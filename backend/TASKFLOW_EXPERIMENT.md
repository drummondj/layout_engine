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
