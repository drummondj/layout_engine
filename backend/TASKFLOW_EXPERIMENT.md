# Taskflow experiment

I would like to explore how to integrate taskflow https://github.com/taskflow/taskflow to re-implement the pipeline, render and instancing modules. Why? Given the good single thread performace of the current solution I would like to see if a thread pool with dynamic task DAG can increase capacity even more.

The goals are:

1. Determine the maximum performance achiveable on a multi-cpu architecture.
2. Develop a consistent way of building data pipelines that is easy for a human to understand and modify. This involves building simple reusable functions that can me composed into a pipeline.
3. Add the ability to profile performance per stage and pipeline.
4. Build a dynamic DAG, for example it should be easy for me to split processing steps per layer.

Please create a separate module in `src/flow` with the basic building blocks for the goals above.

## Status

`src/flow` exists: `flow::stage` (named-task convention, goal 2),
`flow::StageProfiler` (per-stage-name call count/total/min/max via
Taskflow's own `tf::ObserverInterface`, goal 3), and
`flow::parallel_by_key` (fans a `std::map<Key, Value>` out into one
dynamically-spawned task per entry and joins the results back into a map -
shaped after `pipeline`'s own `std::map<ViewLayerId, ...>` grouping, goal
4). Tested in `src/flow/tests/flow_test.cpp`. Doesn't touch `pipeline`/
`render`/`instancing` yet - that's a follow-up once these building blocks
and the numbers below justify it.

`src/flow/benchmarks/flow_benchmark.cpp` answers goal 1 with a synthetic
workload (1M items, transcendental-op cost per item, grouped into 64
"layers" - a stand-in for real per-shape stage cost, not the real
`Pipeline`/`Renderer` classes) on a 10-core Apple Silicon dev machine
(Debug build, so absolute numbers aren't meaningful - relative scaling is):

| workers | wall time | vs. serial |
|---|---|---|
| serial (1 thread, no Taskflow) | 95.6 ms | 1.0x |
| 1 | 94.7 ms | 1.0x |
| 2 | 48.0 ms | 2.0x |
| 4 | 26.7 ms | 3.6x |
| 8 | 21.3 ms | 4.5x |
| 10 | 19.7 ms | 4.9x |

Real (if sub-linear past 4 workers, likely reflecting this machine's
4 performance + 6 efficiency core split, not a `flow` overhead problem) -
worth re-running on the actual Linux target hardware before drawing
conclusions about real-world ceiling.

## Real algorithm, not synthetic

`src/flow/pipeline/` is a fresh, standalone reimplementation of
`generate_shapes`/`filter_by_viewport_and_size` (ported from
`src/pipeline/stages/` as reference, not by editing or including it -
`flow` stays fully self-contained, no coupling into `pipeline`/
`instancing`/`render`), parallelized with `flow::parallel_chunks`. Same
1,000,000-item scale, built directly via the database's `create_*` API
(100,000 Terminals + 900,000 Obstruction shapes across 4 Obstructions -
deliberately flattened by shape, not by owning object, since a real
macro's whole `OBS` block is typically one Obstruction owning many
Shapes and chunking by id alone would give zero parallelism there).
Debug build, same 10-core machine as above:

| workers | GenerateAbstractShapes | FilterByViewportAndSize |
|---|---|---|
| 1 | 2093 ms | 686 ms |
| 2 | 1502 ms (1.4x) | 576 ms (1.2x) |
| 4 | 1106 ms (1.9x) | 454 ms (1.5x) |
| 8 | 975 ms (2.1x) | 421 ms (1.6x) |
| 10 | 1033 ms (2.0x) | 482 ms (1.4x) |

Real speedup, but far more modest than the synthetic benchmark's 4.9x at
10 workers - the real per-item work here (mostly small vector copies,
`unordered_map` bookkeeping, a handful of geometry calls) is much
cheaper and more allocation-heavy per item than the synthetic
transcendental-math stand-in, so per-chunk overhead (task spawn, result
merge under `parallel_by_key`'s mutex, and normal allocator/heap
contention across threads) eats a larger share, and both curves peak
around 8 workers rather than scaling to the full 10. A real signal that
this workload's ceiling is well short of "core count," not a bug -
worth keeping in mind before deciding whether integrating this into the
real `Pipeline` is worth the complexity.

## Wiring the two stages into a real DAG (goal 4, literally)

`generate_shapes` and `filter_by_viewport_and_size` were being called as
two separate top-level computations (each stage owned a private
`tf::Taskflow` + `executor.run(...).wait()` internally) - not actually
wired together as a graph, just sequenced by ordinary C++. Asked to wire
them with `.precede()` instead, which surfaced a real Taskflow
constraint worth recording:

**A worker thread blocking on `executor.run(taskflow).wait()` for a
*second* taskflow submitted to the *same* executor it's already running
on is Taskflow's own documented deadlock risk** (`tf::Executor::corun`'s
doc comment literally shows this exact anti-pattern in a code sample,
commented out, captioned "will introduce deadlock"). Once
`GenerateAbstractShapesStage`/`FilterByViewportAndSizeStage` became
tasks inside an outer, `.precede()`-wired taskflow, their own internal
`executor.run().wait()` calls would have hit exactly that.

Fix: every piece in `src/flow/` that fans out work now takes a
`tf::Subflow&` (`parallel_chunks`, both stages' `run()`) instead of
owning its own `tf::Executor`/`tf::Taskflow` - a Subflow only ever adds
tasks to a graph that's *already* running, so there's no second
top-level `run()` call to deadlock on, no matter how deep the nesting
goes. `flow::run_subflow(executor, fn)` (`flow.hpp`) is now the *one*
place a real top-level `executor.run(...).wait()` happens, called from
the caller's own ordinary (non-worker) thread. `GenerateAndFilterPipeline`
(`src/flow/pipeline/generate_and_filter_pipeline.hpp`) is the actual
answer to "wire these up with precede": one `tf::Taskflow`, two
`tf::Task`s (each a `tf::Subflow`-based dynamic task), `generate_task
.precede(filter_task)` - a real dependency edge, established once
instead of at every call site. The two stages share their data (there's
no way to pass a return value across a `.precede()` edge - it only
orders execution) via two pointers the class owns, written by
`generate_task`'s lambda and read by `filter_task`'s.

This also caught a second, related bug during testing: a single
`tf::Subflow` can only be joined once (`sf.join()` - "once the subflow
is joined, it is considered finished... you may not modify the subflow
anymore"), and `GenerateAbstractShapesStage::run()` was calling
`parallel_chunks` *twice* against the same `subflow` parameter (once for
Terminals, once for Obstruction shapes) - the second call tried to add
tasks to an already-joined Subflow. Reproduced as a real hang in
`FlowPipelineStagesFixture.GenerateShapesLargeFixtureIsCorrectAcrossChunks`
the moment the chunked path first ran under the new Subflow-based code,
not caught by compilation. Fixed by giving each phase its own
freshly-spawned child task (Taskflow hands every dynamic task its own
private Subflow) - with the bonus that, with no `.precede()` between
them, Terminals and Obstructions now generate concurrently with each
other too, not just internally chunked.

`BM_Flow_GenerateAndFilterPipeline` (new, alongside the two isolated-stage
benchmarks) measures the real end-to-end DAG: ~2.6s at 1 worker down to
~1.3s at 8 workers on the same 10-core machine (~2x) - roughly
generate's cost plus filter's cost, as the `.precede()` ordering
requires, with no double-counting or missing work from the nested
dynamic tasking.

## Re-architecture: per-layer/per-object-type task granularity instead of intra-stage chunking

The chunked design above only ever got ~2x at 8 workers on the real
algorithm - the read was that per-chunk overhead (task spawn,
`parallel_by_key`'s mutex-protected merge) was eating too much of the
win at that granularity. New direction: no chunking *within* a stage at
all - each task runs single-threaded start to finish, and parallelism
comes entirely from having many small, independent tasks: one
`generate -> filter` chain per *(physical layer, object type)* pair,
built as one static (non-`Subflow`) graph, plus a final `collect_shapes`
task. `src/flow/pipeline/generate_terminals_on_layer_stage.hpp`/
`generate_obstructions_on_layer_stage.hpp`/`generate_vias_on_layer_stage.hpp`
replace `generate_abstract_shapes_stage.hpp`; `GenerateAndFilterPipeline`
now builds this per-layer graph directly off `view_layers.rows()` rather
than owning one shared pair of stages.

A via/cut layer (e.g. VIA12) is treated as its own layer-list item, not
folded into whichever routing layer references it - `GenerateTerminalsOnLayerStage`/
`GenerateObstructionsOnLayerStage` do zero via resolution now;
`GenerateViasOnLayerStage` owns it entirely for a given named via,
walking both Terminals and Obstructions and emitting geometry that
naturally spans the routing layer(s) above/below. All 762 tests pass,
including the ported cross-layer via test (`GenerateAndFilterPipelineResolvesCrossLayerViaEndToEnd`)
and a real deadlock this refactor caught and fixed along the way: two
`parallel_chunks` calls sharing one `tf::Subflow` (a `Subflow` can only
be joined once) hung an actual test run before the fix.

**The real numbers are a genuinely mixed result, not a clean win** - the
new design's own parallelism ceiling is bounded by `2 * layer_count`
independent chains, unlike the chunked design, which scaled with
`hardware_concurrency()` regardless of layer count. `flow_pipeline_stages_benchmarks`
now sweeps both worker count and layer count (2, matching the original
M1/M2 stress fixture, and 8, a more realistic metal-stack count) to
check that hypothesis against real data rather than just asserting it:

| workers | 2 layers | 8 layers |
|---|---|---|
| 1 | 2980 ms | 3471 ms |
| 2 | 2144 ms | 2244 ms |
| 4 | 1835 ms | 1680 ms |
| 8 | 1716 ms | 1520 ms |
| 10 | 1703 ms | 1389 ms |

Confirmed: at 2 layers, speedup plateaus around 4 workers (1.75x by 10
workers - the ~4-chain ceiling); at 8 layers (16 chains available),
scaling keeps improving through 10 workers (2.5x). More layers really
does unlock more usable parallelism, as the per-layer design predicts.

**But** - comparing directly against the *chunked* design's own
end-to-end number on the same 2-layer stress fixture (previous section:
~2633 ms at 1 worker, ~1314 ms at 8 workers, ~2.0x) - the new per-layer
design is **both slower in absolute terms and scales worse** on this
fixture (2980→1716 ms, 1.75x) than the design it replaced. Two real
costs the per-layer split adds that the chunked design didn't have:
1. **Redundant per-layer walks** - every routing-layer task still
   iterates every Terminal/Obstruction in the Abstract, just to check a
   Shape's own `.layer` (see this file's earlier "via handling" note) -
   total walk-and-compare cost scales with `layer_count x item_count`,
   even though the real per-matching-item work doesn't. This is very
   likely why 8 layers (3471 ms at 1 worker) is *slower* than 2 layers
   (2980 ms at 1 worker) at a *single* worker, despite doing the exact
   same amount of real generation work either way.
2. **A hard task-count ceiling** the chunked design never had - a
   design with few metal layers (this stress fixture's 2, but also many
   real small designs) simply can't produce enough independent chains
   to use every core, no matter how cheap each task is.

Net finding: per-layer granularity is a real, measurable improvement
over the chunked design *only* once a design has enough layers to
supply enough independent chains to outrun its own redundant-walk cost
- and even at 8 layers here, it still hasn't beaten the chunked design's
absolute numbers on 2 layers. A hybrid (per-layer chains, each *also*
internally chunked once it clears its own size threshold) would very
plausibly beat both, but that's a further iteration, not this one -
recorded here as the natural next experiment rather than attempted
speculatively.

## Pre-process step: bucket shapes by layer once, instead of a redundant walk per layer

Tried the obvious fix for the "redundant per-layer walks" finding above:
`ShapesByLayerIndex` (`src/flow/pipeline/shapes_by_layer_index.hpp`) does
one single-threaded pre-pass over the Abstract, bucketing every Terminal/
Obstruction Shape by its own `.layer` into a `std::map<LayerId,
vector<pair<...>>>`, and every via reference by `via_name` into a
similar map (closing the same gap for `GenerateViasOnLayerStage`, which
had the identical walk-everything-and-skip problem, just keyed on via
name instead of routing layer). `GenerateTerminalsOnLayerStage`/
`GenerateObstructionsOnLayerStage`/`GenerateViasOnLayerStage` now consume
this index's own pre-bucketed lists directly instead of walking
`Root`'s Terminals/Obstructions and filtering themselves - dropped
`abstract_id` from all three signatures entirely, composing their own
cache key via the index's `.version()` plus `view_layers.generation()`
(the index is deliberately *not* keyed on ViewLayerSet at all - see its
own comment for why those two triggers are genuinely orthogonal). Cached
the same way as everything else here (`{abstract_id,
root.mutation_version()}`), so a viewport-only call still doesn't
re-bucket.

**Result: a wash, not a win** - within noise of the un-indexed numbers
above, and measurably *worse* at higher worker counts on 8 layers (1389 ms
→ 1713 ms at 10 workers):

| workers | 2 layers | 8 layers |
|---|---|---|
| 1 | 3134 ms | 3359 ms |
| 2 | 2521 ms | 2309 ms |
| 4 | 2195 ms | 1739 ms |
| 8 | 1974 ms | 1656 ms |
| 10 | 1849 ms | 1713 ms |

The real per-item comparison the redundant walk did (`shape.layer !=
physical_layer`) was already a single cheap integer compare, and every
copy of it ran *inside* an already-parallel task - so the "wasted" total
work was never on the critical path, it was spread across idle cores.
The index build replaces that with a single-threaded pre-pass doing real
work per item (a `std::map` lookup-or-insert plus a `vector::push_back`,
per Shape, before any per-layer task can even start) - cheaper in total
operations, but now a genuine serial bottleneck gating the entire graph,
exactly the kind of thing Amdahl's law punishes regardless of how many
workers are available downstream of it. (Also tried skipping the
`std::set` construction in the via-bucketing path for the ~all-Shapes-
have-no-vias common case - real, measured overhead on its own, but not
enough to change the overall picture.)

Net finding: eliminating redundant *total* work is not automatically a
win in a task-parallel design - it only helps if the elimination itself
stays parallel (or at least cheap enough not to matter serially).
Reverted (`ShapesByLayerIndex` and every call site back to the
walk-and-skip approach) once the numbers came back a wash-or-worse
rather than the clear win the "redundant walks" framing predicted -
worth knowing before reaching for "pre-index everything" as a default
move next time.

## Correction: every number above was measured wrong for a vs.-the-real-Pipeline comparison

Asked directly "are we considerably slower than the original
(non-flow) benchmarks" - and every number recorded in this file so far
turns out to have two real problems that make it invalid for *that*
comparison specifically (the internal A/B comparisons - chunked vs.
per-layer, 2 vs. 8 layers, indexed vs. not - stayed valid, since both
sides of each of those were measured the same flawed way):

1. **Every `flow_pipeline_stages_benchmarks` run this whole session was
   a Debug build.** This project's own rule (`CLAUDE.md`) is explicit:
   "Debug timings aren't meaningful." Never caught because I never
   compared against a same-session Release run of the real `Pipeline`
   until asked to.
2. **The synthetic stress fixture used a plain RECT for every item.**
   The real `stress_data.hpp` alternates RECT/POLYGON/PATH per item
   (`write_geometry_item`) - a PATH shape costs a real
   `Geometry::path_to_polygons` call (~768ns/call,
   `BM_PathToPolygonsSingleCall`), so an all-RECT fixture silently
   skipped roughly a third of the real per-item cost. First symptom:
   a Release run of the flow pipeline came back *faster* than the real
   single-threaded `Pipeline` even at 1 worker - suspicious enough to
   check rather than report. Fixed in `pipeline_stages_benchmark.cpp`'s
   own `stress_data()` to cycle RECT/POLYGON/PATH exactly like the real
   fixture.

**Corrected comparison** - same machine, same session, Release build,
1,000,000 shapes, `pipeline_benchmarks`' real `BM_GenerateShapes` +
`BM_FilterByViewportAndSize` (743 ms + 18.2 ms = **761 ms**, single-
threaded, today's actual `Pipeline`) against
`flow_pipeline_stages_benchmarks`' `BM_Flow_GenerateAndFilterPipeline`
with matching RECT/POLYGON/PATH geometry:

| workers | 2 layers | 8 layers |
|---|---|---|
| 1 | 885 ms (1.16x) | 939 ms (1.23x) |
| 2 | 785 ms (1.03x) | 772 ms (1.01x) |
| 4 | 625 ms (0.82x) | 615 ms (0.81x) |
| 8 | 612 ms (0.80x) | 509 ms (0.67x) |
| 10 | 608 ms (0.80x) | 486 ms (0.64x) |

(x = ratio to the real Pipeline's 761 ms; below 1.0 means faster.)

So: **no, not considerably slower** - the opposite, once measured
correctly. At 1 worker the flow-based per-layer DAG is a real but modest
~16-23% slower than today's single-threaded `Pipeline` (Taskflow
scheduling overhead, plus - at 8 layers - more of the per-layer
redundant-walk cost this file's earlier entries already found), but with
real parallelism it pulls ahead: ~20% faster at 8-10 workers on the
original 2-layer fixture, ~33-36% faster at 8-10 workers on 8 layers.
This is the number that should have anchored every earlier comparison in
this file - the relative findings above (per-layer vs. chunked, 2 vs. 8
layers, indexed vs. not) still hold, but their *absolute* magnitudes
should be read as "under a flawed-for-external-comparison but internally
consistent measurement," not as real wall-clock costs.

## A second, separate pipeline: parallel instance-picture rendering

A different real bottleneck from the abstract-shapes work above:
hierarchical instance rendering (`src/instancing/InstanceRenderer`),
tested against a real industrial design,
`test_data/ispd19_test10/` - 899,404 placements, 18 LEF layers, but only
70 distinct referenced cell types. `InstanceRenderer`'s own
`resolve_design_picture` cache already means each distinct design's
picture is built once however many times it's placed, so the real
suspected cost at 900K placements was `build_layout_picture_uncached`'s
own single-threaded per-placement loop.

Fork boundary: only `InstanceRenderer`'s own orchestration (the
resolve/cache/build algorithm) got a fresh, self-contained port,
`flow::ParallelLayoutPictureBuilder`
(`src/flow/instancing/parallel_layout_picture_builder.hpp`), keeping
`src/flow/pipeline/` (the work above) completely untouched. Everything
below that orchestration layer -
`GenerateAbstractShapesStage`/`GenerateLayoutShapesStage`/
`FilterByViewportAndSizeStage`/`FilterByLayerVisibilityStage`
(`src/pipeline/stages/`), `BuildLayoutPictureStage`/`draw_helpers.hpp`
(`src/render/`) - is real, already-tested, Skia-correct code with no
`src/instancing`-specific logic in it, so the new module links `pipeline`
and `render` directly and calls it unmodified, exactly as
`InstanceRenderer` itself already does; `src/instancing/`'s own real code
is never edited, read only as reference. New CMake INTERFACE library
`flow_instancing`, mirroring `src/instancing/`'s own "allowed to link
both pipeline and render" role.

### First attempt: a 3-phase design, and a bottleneck that wasn't where expected

The first design ran three sequential `executor.run().wait()` phases:
resolve every distinct design's own picture in parallel (Phase 1), chunk
the 899,404 placements across workers and draw each chunk into its own
sub-picture in parallel (Phase 2), then one final serial compose step
(Phase 3) that drew the Layout's own direct content
(`GenerateLayoutShapesStage::run()` - die area, rows, tracks, gcell
grids, blockages, routed net geometry, ports) plus every chunk's
sub-picture.

Real `flow::StageProfiler` numbers against `ispd19_test10` (Release
build, 10-core machine):

```
InstanceRenderer::build_layout_picture (real, serial): 370.36 ms
ParallelLayoutPictureBuilder::run (workers=10): 318.83 ms
    compose_final_picture    calls=     1  total=   296.87 ms
    draw_placement_chunk     calls=    10  total=    90.01 ms
    find_distinct_designs    calls=     1  total=     8.81 ms
    resolve_distinct_design  calls=    70  total=     9.68 ms
```

Only ~1.2x speedup - far less than the per-placement parallel work
alone would predict. The profiler broke down why: `compose_final_picture`
cost ~290-297 ms **regardless of worker count**, dwarfing the ~90 ms the
actually-parallelized `draw_placement_chunk` work cost in total. The cost
wasn't the per-placement drawing loop this pipeline set out to
parallelize (that part worked, and is fast) - it was
`GenerateLayoutShapesStage::run()`, called serially inside Phase 3,
synthesizing on-the-fly geometry for the real DEF's 1,281 `ROW`
statements (each expanding to a full row of `SITE`-spaced rects) and 18
`TRACKS` statements. That work has **no dependency on placement
resolution at all** - it was only serial because the 3-phase design put
it last, not because it needed to be.

### Restructure: let the DAG express the real independence

Rebuilt `run()` as one `tf::Taskflow` instead of three sequential
barriers, with real `.precede()` edges instead of ordering-by-phase:

- `generate_own_shapes` (the former Phase 3's `GenerateLayoutShapesStage`
  call, moved earlier) - a static task with **no** precedence edge to
  resolve/draw, so Taskflow's own scheduler is free to run it
  concurrently with everything below.
- `resolve_and_draw` - a dynamic (`tf::Subflow`-based) task, since the
  distinct-design count and chunk count are only known once it starts
  running: it dedups distinct designs, spawns one `resolve_distinct_design`
  sub-task per design, then a nested `draw_all_chunks` sub-task (its own
  fresh `Subflow`, since a `Subflow` can only be `join()`ed once -
  reusing one for two rounds of dynamic fan-out was the exact double-join
  hazard already found and fixed in the abstract-shapes work above) that
  chunks and draws placements once every design is resolved.
- `compose_final_picture` - now genuinely cheap: `.precede()`d by both
  `generate_own_shapes` and `resolve_and_draw`, it just draws the
  already-computed own-shapes plus every chunk's already-recorded
  sub-picture; no `GenerateLayoutShapesStage` call left in it at all.

Re-measured, same machine, same data, Release build:

```
InstanceRenderer::build_layout_picture (real, serial): 414.87 ms
ParallelLayoutPictureBuilder::run (workers=1):  373.32 ms
ParallelLayoutPictureBuilder::run (workers=2):  343.68 ms
ParallelLayoutPictureBuilder::run (workers=4):  295.71 ms
    compose_final_picture    calls=     1  total=    34.26 ms
    draw_all_chunks          calls=     1  total=    14.14 ms
    draw_placement_chunk     calls=     4  total=    65.52 ms
    generate_own_shapes      calls=     1  total=   255.45 ms
    resolve_and_draw         calls=     1  total=    31.17 ms
    resolve_distinct_design  calls=    70  total=     3.25 ms
ParallelLayoutPictureBuilder::run (workers=8):  297.84 ms
ParallelLayoutPictureBuilder::run (workers=10): 298.57 ms
```

The overlap is real and visible in the numbers: at 4 workers,
`resolve_and_draw`'s own wall time is only 31 ms even though its
`draw_placement_chunk` sub-tasks did 65 ms of actual work - it finished
while `generate_own_shapes` (255 ms) was still running, i.e. genuinely
concurrent, not serialized. `compose_final_picture` dropped from ~290 ms
to ~34 ms, since it no longer does any shape generation itself. At 1
worker there's no real concurrency to be had (one worker can't run two
tasks at once), so the numbers there are close to the plain sum of the
parts - expected, and a useful sanity check that the "overlap" claim
isn't just noise.

Overall: 414.87 ms serial baseline → ~296-298 ms at 4-10 workers, a real
but bounded **~1.4x speedup** (up from ~1.2x before the restructure).
The remaining ceiling is `generate_own_shapes` itself: single-threaded,
doesn't shrink with worker count, and once `resolve_and_draw` finishes
well inside its shadow (any worker count ≥ 2), wall time is essentially
`generate_own_shapes + compose_final_picture` no matter how many workers
are thrown at placement drawing. Parallelizing the row/track/gcell-grid
synthesis itself (e.g. per-row) would be the next real lever if more
speedup is wanted here - not attempted, since it wasn't part of what was
asked (splitting instance-picture generation into threads, which this
pipeline now does) and is a new scope decision on top of what was
approved.

`src/flow/tests/parallel_layout_picture_builder_test.cpp` (7 tests, all
passing) covers `ParallelLayoutPictureBuilder`: single/rotated-placement
correctness, a 40-placement fixture forcing multiple chunks across 4
workers, a nested-Layout fixture proving the serial-recursion fallback,
an unresolved-reference-design fixture, the Layout's own direct content
(the `generate_own_shapes`/`compose_final_picture` split above) drawing
correctly alongside placements, and - the strongest proof available - a
true pixel-diff A/B against the real, unmodified `InstanceRenderer` at
1/2/4 workers (every one of `run()`'s own chunk-count threshold branches),
byte-identical in all three. One test bug found while writing these: an
80-dbu leaf fell below `ParallelLayoutPictureBuilder`'s own 100-dbu
min-visible-instance threshold at scale=1.0 and silently collapsed to an
unfilled outline rect - the region-scan check happened to sample only
the unfilled center, a real "test asserted something true for the wrong
reason" trap, not a builder bug; fixed by sizing the fixture's leaf
comfortably above the threshold instead. Full suite (777 tests) green.

### A quick check: min_visible_instance_pixels=1.0 on both builders

Asked directly whether the default 100-dbu min-visible-instance threshold
(both `InstanceRenderer` and `ParallelLayoutPictureBuilder` share the same
knob/default) was understating the real per-placement cost, since most of
`ispd19_test10`'s own real standard-cell instances collapse to a cheap
unfilled outline dot at `kScale=0.001` rather than resolving/drawing real
content. Lowered to 1.0 on both (`InstanceRenderer::
set_min_visible_instance_pixels`, already a real public setter, plus a new
matching one added to `ParallelLayoutPictureBuilder`) and re-ran:

| workers | serial baseline | parallel |
|---|---|---|
| 1 | 435.79 ms | 438.63 ms |
| 2 | - | 307.86 ms (1.42x) |
| 4 | - | 301.25 ms (1.45x) |
| 8 | - | 323.50 ms (worse than 4) |
| 10 | - | 338.26 ms (worse than 4) |

Two findings: overall wall time barely moved from the default-threshold
run (`draw_placement_chunk`'s own summed work went from ~90ms to
170-834ms total, but `generate_own_shapes`, ~260-294ms single-threaded,
was still the dominant serial cost either way, with enough slack at 2-4
workers to absorb the extra real drawing work underneath it); and
speedup gets *worse*, not better, past 4 workers - real per-placement
`BuildLayoutPictureStage` work (not a cheap dot) means 8-10 threads now
contend for CPU/memory bandwidth on real work, a genuine negative-scaling
result invisible at the default threshold. (Caveat: nested per-task
profiler sums, e.g. `draw_placement_chunk` totaling 834ms under a parent
`resolve_and_draw` measured at only 24ms, don't add up cleanly - this is
Taskflow's own work-stealing during `subflow.join()` (a blocked worker
can steal and execute other ready tasks meanwhile), not a bug. Wall-clock
totals are trustworthy; nested per-task sums aren't directly additive
here.)

### Splitting generate_own_shapes itself, per object type

The remaining ceiling identified above - `generate_own_shapes`, ~250-290ms,
single-threaded, dominating wall time regardless of placement-side worker
count - was assumed (not yet confirmed) to be row synthesis, given
`ispd19_test10` has 1,281 `ROW` statements. Checking the real
`GenerateLayoutShapesStage::append_row_shapes` disproved that: it
synthesizes exactly one aggregate bbox per Row (`row_footprint_bbox`,
`src/core/row_geometry.hpp`), not per-site - O(row count), genuinely
cheap. The real stage walks 8 independent object-type sources serially
(die area, blockages, routes, physical ports, rows, tracks, gcell grid,
regions) with no cross-dependency between them, so - per the user's own
ask, and after confirming the fork-vs-touch-real-code trade-off with them
- `flow::ParallelGenerateLayoutShapesStage`
(`src/flow/instancing/parallel_generate_layout_shapes_stage.hpp`) forks
just the orchestration (same "fork orchestration only" boundary as
`ParallelLayoutPictureBuilder` itself; the real, genuinely shared
utilities it's built on - `row_footprint_bbox`, `via_shapes.hpp`'s
`append_via_shapes`, `Geometry::path_to_polygons` - stay linked, not
duplicated) into one dynamic Taskflow sub-task per object type, each
writing its own local vector, merged at the end. `generate_own_shapes`
itself became a dynamic (`tf::Subflow`-taking) task to host these 8
sub-tasks, still with no precede edge to the placement-resolve/draw
chain, so the whole thing still runs concurrently with Phase 1/2 as
before.

Real numbers, same data, Release build - and the actual per-type
breakdown the profiler now exposes:

```
generate_own_shapes (workers=4): 248.57 ms
    gen_diearea     0.00 ms
    gen_blockages   0.00 ms
    gen_ports       0.69 ms
    gen_regions     0.00 ms
    gen_rows        0.17 ms
    gen_gcellgrid   6.14 ms
    gen_routes      6.59 ms
    gen_tracks    254.59 ms   <- the real bottleneck
```

Not rows - **`TRACKS` synthesis**, consistently ~241-280ms across every
worker count, dwarfing the other 7 sources combined (under 10ms total).
`append_track_shapes` (read as reference, not edited) synthesizes
`track->count` zero-width guide-line `Path`s per `TrackId`, then - for
*each* of that track's own `layer_names` - copies the whole line set and
runs `compute_path_outlines` (a real `Geometry::path_to_polygons` buffer
call per line) again; a real chip's routing-grid `TRACKS` statements at
fine pitch plausibly synthesize tens of thousands of lines per statement
across several layers each, and this DEF has 18 such statements.

Splitting `generate_own_shapes` into 8 object-type tasks did NOT
meaningfully change overall wall time (~300-325ms, same as before this
change) - expected once the real per-type breakdown is visible: one
single task (`gen_tracks`) still accounts for effectively the whole
`generate_own_shapes` cost, so parallelizing *across types* has nothing
left to gain once the other 7 are already near-zero. The 8-way split did
do its actual job, though - it turned a single opaque ~260ms number into
a real, correctly-attributed breakdown, confirming the earlier
hypothesis (rows) was wrong and pointing at the actual cost driver. A
further win here would mean splitting `gen_tracks` itself (e.g. per
`TrackId`, or per `layer_name` within one), or questioning whether
`compute_path_outlines`' full buffered-polygon computation is even used
for a zero-width guide line ("drawn as a thin centerline stroke
regardless of width" per that stage's own comment) - neither attempted,
since both are new scope beyond what was asked.

Full suite (777 tests, including the pixel-diff A/B) stays green after
this change - `ParallelGenerateLayoutShapesStage`'s re-derived per-type
logic produces byte-identical output to the real, unmodified
`GenerateLayoutShapesStage`.

### Disabling TRACKS synthesis outright

Given the finding above, asked directly: a real user only looks at TRACK
geometry zoomed in on a real routing question, not in this pipeline's
bird's-eye-view target use case - so `ParallelGenerateLayoutShapesStage`'s
`append_track_shapes` was changed to a deliberate no-op (the `gen_tracks`
task removed entirely, not left as an empty task) rather than optimized.
A real, intentional divergence from the real `InstanceRenderer` - covered
by a new regression test
(`TracksAreDeliberatelyNotGeneratedUnlikeTheRealInstanceRenderer`) that
proves the real renderer still draws a track line while this pipeline
doesn't, so a future re-enable shows up as an intentional test change,
not silent drift. Full suite (778 tests) green.

Real numbers, same data, Release build - and this is where the payoff
actually shows up:

```
InstanceRenderer::build_layout_picture (real, serial): 427.39 ms
ParallelLayoutPictureBuilder::run (workers=1):  124.82 ms (3.4x)
ParallelLayoutPictureBuilder::run (workers=2):   78.57 ms (5.4x)
ParallelLayoutPictureBuilder::run (workers=4):   69.76 ms (6.1x)
ParallelLayoutPictureBuilder::run (workers=8):  105.60 ms (4.0x - worse than 4)
ParallelLayoutPictureBuilder::run (workers=10): 109.71 ms (3.9x - worse than 4)
```

`generate_own_shapes` dropped from ~250-290ms to **under 7ms** at every
worker count - it was never really "generate the Layout's own content"
that was expensive, it was TRACKS specifically, exactly as the per-type
breakdown showed. With that removed, `resolve_and_draw` (the real
placement-resolve/draw work this pipeline set out to parallelize in the
first place) is now genuinely the critical path.

**Correction - the 6.1x above was measured wrong.** Asked directly "how
does that compare to the original InstanceRenderer without tracks" -
the 427.39ms serial baseline above still included the real
InstanceRenderer's own TRACKS cost (real, unmodified code - never
edited), while `ParallelLayoutPictureBuilder` skips TRACKS by
construction. Comparing a baseline that still pays for tracks against a
builder that doesn't is the same shape of mistake this file's own
earlier "Correction" entry already caught once (Debug build / RECT-only
geometry) - an apples-to-oranges number, not a real one. Fixed by
removing every real `Track` from the `Root` (`root.clear_track()`,
`flow_profile_instancing`'s own tool code, not either renderer) before
*either* renderer runs, so both are measured against identical,
track-free data:

```
InstanceRenderer::build_layout_picture (real, serial, no tracks): 130.88 ms
ParallelLayoutPictureBuilder::run (workers=1):  118.60 ms (1.10x)
ParallelLayoutPictureBuilder::run (workers=2):   72.21 ms (1.81x)
ParallelLayoutPictureBuilder::run (workers=4):   71.13 ms (1.84x)
ParallelLayoutPictureBuilder::run (workers=8):  100.19 ms (1.31x - worse than 4)
ParallelLayoutPictureBuilder::run (workers=10): 111.84 ms (1.17x - worse than 4, nearly back to serial)
```

The real, honest number is **~1.84x at 4 workers**, not 6.1x - still a
genuine, worthwhile win (and InstanceRenderer's own 130.88ms, down from
427.39ms, confirms TRACKS really did cost ~296ms in the real,
unmodified renderer too - not an artifact of this fork's own
measurement), but nowhere near what the unfair comparison implied. The
same negative-scaling pattern found earlier reappears past 4 workers
(8/10 workers regress to 100-112ms, nearly back to serial - real
CPU/memory-bandwidth contention on `draw_placement_chunk`'s own
now-dominant work) - worth knowing if a real caller picks a worker
count without measuring first.

### Why does more than 4 workers get worse - and why does 4 barely beat 2?

Investigated directly rather than guessed - two things stood out: 8/10
workers actively *regress* (not just plateau) past 4, and even 4 workers
(71.13ms) barely beats 2 (72.21ms) despite double the worker count.

This dev machine (Apple M4, `sysctl hw.perflevel0/1.physicalcpu`) has an
asymmetric core layout - **4 performance cores, 6 efficiency cores**, not
10 uniform cores - workers=4 landing exactly on the P-core count was the
first, obvious suspect.

Checked with a standalone synthetic probe: N `std::thread`s doing pure
CPU-bound arithmetic with zero shared memory, no Skia/Taskflow involved.
Result: scales *cleanly* all the way to 10 threads (3240ms at 1 thread ->
485ms at 10), no cliff, no regression - just the expected diminishing
marginal return once E-cores start doing their share. This rules out raw
core topology as *the* explanation for an actual regression (8/10 workers
slower than 4) - topology alone predicts a plateau, not a reversal, and
it doesn't explain why 4 barely beats 2 either (a pure P/E-core effect
should still show a real gain doubling 2->4, both comfortably inside 4
P-cores).

Checked a second, more targeted hypothesis: `draw_placement_chunk`'s own
`resolve_picture(design_id)` returns an `sk_sp<SkPicture>` **by value**
from the resolved-designs map - a copy that atomically increments the
real `SkPicture`'s refcount, then decrements it again when the chunk's
own `instances` vector is destroyed at the end of the call. `ispd19_test10`
has 899,404 placements referencing only **70 distinct designs** (~12,850
placements per design on average) - many concurrent chunk tasks are very
likely touching the *same* small set of 70 atomic refcounts simultaneously,
and that contention only grows with worker count. A second synthetic
probe confirmed this exactly: N threads doing `fetch_add`/`fetch_sub`
against 70 shared atomics (same distinct-count as the real data) vs. the
identical total work spread across N *private*, unshared atomics:

```
SHARED (70 contended refcounts):    496ms (1 thread) -> 750ms (2) -> 1463ms (4) -> 8809ms (10 threads)
PRIVATE (zero sharing):            1290ms (1 thread) -> 646ms (2) ->  360ms (4) ->  184ms (10 threads) - a clean 7x speedup
```

This lines up with both real observations at once: the SHARED case is
already meaningfully worse than PRIVATE at just 2-4 threads (contention
starts eating the gain immediately, explaining why 4 barely beats 2), and
gets *catastrophically* worse by 8-10 (contention compounds
non-linearly, unlike raw compute, because more cores fighting over the
same cache line means more cache-coherence (MESI) traffic, not just more
work) - explaining the outright regression there. Core topology likely
compounds this further at 8-10 workers (consistent with M4's own 4
P-cores), but the atomic refcount contention is the dominant mechanism
across the whole curve, not just the tail.

Two candidate fixes were considered: (1) store a raw `SkPicture*` in the
resolved-designs map instead of `sk_sp`, or (2) chunk placements *by
referenced design* instead of by flat index range. (1) was checked first
and traced through carefully rather than implemented blind - it turns out
**not to help at all**: `BuildLayoutPictureStage::ResolvedInstance::picture`
(real, unmodifiable render code) is a genuinely-owning `sk_sp<SkPicture>`
per placement drawn - its destructor unconditionally decrements when the
chunk's own `instances` vector is destroyed, so one real atomic ref/unref
pair per placement is *structurally required* by the real API regardless
of what type the lookup map itself stores; moving the raw pointer's home
doesn't remove that requirement (`SkRefCnt`'s own counter is private, no
public "batch ref by N" exists to avoid it). Storing raw pointers in the
map would have been a no-op change dressed up as a fix - not implemented.

### Fix: chunk placements by referenced design, not by flat index range

(2) directly attacks the real problem - not the atomic op *count* (fixed
by the real API, per above) but *which threads* touch a given design's
refcount. `draw_all_chunks_task`'s Phase 2 now buckets `placements` by
`reference_design` first, then greedily bin-packs whole buckets
(largest-first, each always assigned to the currently least-loaded
chunk) across `chunk_count` chunks - guaranteeing one design's own
placements, and therefore its own `sk_sp<SkPicture>` refcount, are only
ever touched by one worker thread, never split across chunks.
`draw_placement_chunk`'s own signature changed from a `[begin, end)`
index range into `placements` to an explicit `chunk_placements` list (no
longer necessarily contiguous in the original insertion order).

The existing `ManyPlacementsAcrossMultipleChunksAllRenderAtTheirCorrectLocation`
test broke silently on this change in a revealing way: it used 40
placements of the SAME design, which - correctly, under the new
bin-packing - all landed in one chunk, since there was only one design
bucket to distribute; the test still passed (its assertions were about
placement correctness, not chunk count) but no longer exercised what it
claimed to. Fixed by using 8 distinct designs (5 placements each) so the
load-balancer has real, independent buckets to spread across chunks -
this is the kind of thing only a real design-plurality fixture catches,
not a single-design one, worth remembering for any future placement/
instancing test.

Real numbers, same data, Release build, tracks still removed from both
for a fair comparison:

```
InstanceRenderer::build_layout_picture (real, serial, no tracks): 132.50 ms
ParallelLayoutPictureBuilder::run (workers=1):  143.51 ms (0.92x - single-thread overhead)
ParallelLayoutPictureBuilder::run (workers=2):   80.34 ms (1.65x)
ParallelLayoutPictureBuilder::run (workers=4):   56.94 ms (2.33x)
ParallelLayoutPictureBuilder::run (workers=8):   60.61 ms (2.19x - barely worse than 4)
ParallelLayoutPictureBuilder::run (workers=10):  62.26 ms (2.13x - barely worse than 4)
```

Two real wins, confirming the contention hypothesis directly: the best
number improved from 71.13ms (flat-index chunking) to **56.94ms**
(1.84x -> 2.33x speedup), and - the more important fix - the earlier
catastrophic regression past 4 workers is essentially gone. 8 and 10
workers are now only slightly worse than 4, not collapsing back toward
serial (100.19ms/111.84ms before this fix). `draw_placement_chunk`'s own
summed work at 8 workers dropped from 676ms (flat chunking, heavy
cross-thread contention) to 184ms (by-design chunking) for the identical
total placement count - direct, measured confirmation that cross-thread
refcount contention, not raw core topology, was the dominant cause of
the earlier regression, and that grouping by design was the correct fix
for it. Full suite (778 tests, including the pixel-diff A/B and the
fixed multi-chunk test) green.
