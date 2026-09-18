#!/usr/bin/env python3
"""Outer driver for the pipeline_stage_benchmark C++ tool.

Runs the fixed 7-point AES test-case matrix
(LINKING_STRATEGY.md-adjacent, see PIPELINE_REFACTOR_BENCHMARK_RESULTS.md
for the existing hand-recorded convention this mirrors), invoking
`pipeline_stage_benchmark` (src/pipelines/benchmarks/pipeline_stage_benchmark.cpp)
once per test case as a subprocess and collecting the CSV rows it prints
to stdout into one combined `<name>.csv`, then rendering `<name>.md`.

Two distinct fixture recipes, not one uniform tile label - see
pipeline_stage_benchmark.cpp's own header comment and aes_5x5.def's own
comment for why:
  - aes_scaling_<label>.def (1x1/2x1/2x2/3x2/3x3): flat, DESIGN "tiled",
    no real hierarchy - hierarchy_depth is always 0.
  - aes_5x5.def (+ AES_1/design_original.def, read first): genuinely
    2-level hierarchical, DESIGN "aes_5x5" placing 25 instances of
    DESIGN "aes" - run at both hierarchy_depth 0 and 1.

Usage:
    pipeline_stage_benchmark.py --name my_run
    pipeline_stage_benchmark.py --name my_run --bin ../build_release/pipeline_stage_benchmark
    pipeline_stage_benchmark.py --name my_run --output-dir /tmp/my_results

Writes <output-dir>/<name>.csv and <output-dir>/<name>.md - output-dir
defaults to benchmark_results/ (created if missing) in the current
working directory.
"""

import argparse
import csv
import subprocess
import sys
from pathlib import Path

CSV_FIELDS = [
    "label",
    "hierarchy_depth",
    "phase",
    "stage",
    "recomputed",
    "wall_ms",
    "cpu_ms",
    "rss_after_mb",
    "rss_delta_mb",
    "swap_after_mb",
    "swap_delta_mb",
    "cache_object_count",
    "cache_bytes",
]

STAGES = ["LayerGeneration", "HierarchyResolver", "ViewportCull", "Rasterize", "Compose"]
PHASES = [
    ("ColdStartZoomFit", "Cold Start Zoom-Fit"),
    ("ZoomIn", "Zoom-In"),
    ("FinalZoomFit", "Final Zoom-Fit"),
]


def build_test_cases(test_data_dir: Path):
    """Returns the fixed 7-combo matrix as a list of dicts, each with the
    exact CLI args pipeline_stage_benchmark.cpp expects."""
    lef = test_data_dir / "ISPD22__final_benchmarks" / "__Nangate" / "NangateOpenCellLibrary.lef"
    cases = []

    for tile in ["1x1", "2x1", "2x2", "3x2", "3x3"]:
        cases.append(
            {
                "label": tile,
                "lef": lef,
                "defs": [test_data_dir / f"aes_scaling_{tile}.def"],
                "top_design": "tiled",
                "hierarchy_depth": 0,
            }
        )

    for depth in [0, 1]:
        cases.append(
            {
                "label": f"5x5 (d={depth})",
                "lef": lef,
                "defs": [
                    test_data_dir / "ISPD22__final_benchmarks" / "AES_1" / "design_original.def",
                    test_data_dir / "aes_5x5.def",
                ],
                "top_design": "aes_5x5",
                "hierarchy_depth": depth,
            }
        )

    return cases


def run_case(binary: Path, case: dict) -> list[dict]:
    args = [
        str(binary),
        "--lef",
        str(case["lef"]),
        "--top-design",
        case["top_design"],
        "--label",
        case["label"],
        "--hierarchy-depth",
        str(case["hierarchy_depth"]),
    ]
    for def_path in case["defs"]:
        args += ["--def", str(def_path)]

    print(f"Running: {case['label']} (hierarchy_depth={case['hierarchy_depth']})...", file=sys.stderr)
    result = subprocess.run(args, capture_output=True, text=True)
    if result.returncode != 0:
        print(result.stderr, file=sys.stderr)
        raise SystemExit(f"pipeline_stage_benchmark failed for {case['label']!r} (exit {result.returncode})")

    rows = []
    reader = csv.reader(result.stdout.strip().splitlines())
    for fields in reader:
        if len(fields) != len(CSV_FIELDS):
            continue
        rows.append(dict(zip(CSV_FIELDS, fields)))
    return rows


def write_csv(rows: list[dict], path: Path) -> None:
    with path.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=CSV_FIELDS)
        writer.writeheader()
        writer.writerows(rows)


def format_duration_ms(ms: float) -> str:
    if ms < 1.0:
        return f"{ms * 1000:.1f} us"
    if ms < 1000.0:
        return f"{ms:.1f} ms"
    return f"{ms / 1000.0:.2f} s"


def format_bytes(n: float) -> str:
    if n < 1024:
        return f"{n:.0f} B"
    if n < 1024**2:
        return f"{n / 1024:.1f} KB"
    if n < 1024**3:
        return f"{n / 1024**2:.1f} MB"
    return f"{n / 1024**3:.2f} GB"


def format_count(n: int) -> str:
    return f"{n:,}"


def format_wall_cell(r: dict | None) -> str:
    """Wall-time table cell for one (test case, stage) - MUST check
    `recomputed`, not just print wall_ms: tbb_core.hpp's own
    MemoizingStage deliberately leaves last_compute_wall_ns() (and every
    other last_compute_* stat) at whatever the *previous* real compute()
    left it on a cache hit (recomputed == "0") rather than resetting it
    to 0 - useful for confirming a cache hit's cache_object_count/bytes
    didn't change, but printing that stale wall_ms unfiltered here would
    make a real cache hit (near-instant in the actual UI - the stage's
    own compute() never runs at all) look exactly as slow as the Cold
    recompute it's inherited the number from. Bug found by the user
    directly (real UI zoom feels far faster than the Zoom-In column
    once showed for LayerGeneration/HierarchyResolver, which should
    cache-hit on every zoom - only viewport/scale changed)."""
    if r is None:
        return "-"
    if r["recomputed"] == "0":
        return "0 ms (cached)"
    return format_duration_ms(float(r["wall_ms"]))


def markdown_table(headers: list[str], rows: list[list[str]]) -> str:
    lines = ["| " + " | ".join(headers) + " |", "| " + " | ".join(["---"] * len(headers)) + " |"]
    for row in rows:
        lines.append("| " + " | ".join(row) + " |")
    return "\n".join(lines)


def generate_markdown(rows: list[dict], labels_in_order: list[str]) -> str:
    by_key = {(r["label"], r["phase"], r["stage"]): r for r in rows}
    sections = []

    for phase_key, phase_title in PHASES:
        sections.append(f"## {phase_title}\n")

        # Wall time: stage rows x test-case columns - matches this
        # repo's own existing PIPELINE_REFACTOR_BENCHMARK_RESULTS.md
        # convention exactly.
        wall_rows = []
        for stage in STAGES:
            row = [stage]
            for label in labels_in_order:
                r = by_key.get((label, phase_key, stage))
                row.append(format_wall_cell(r))
            wall_rows.append(row)
        sections.append(markdown_table(["Stage", *labels_in_order], wall_rows))
        sections.append("")

        # Cache size: stage rows x test-case columns, one table for
        # object count and one for bytes - the per-stage cache footprint
        # is exactly the "how much memory is used for caching and how
        # many objects" ask, and genuinely varies by stage (Rasterize's
        # own cache is tiny next to HierarchyResolver's), so it stays a
        # full grid like wall time rather than collapsing to one row.
        cache_obj_rows = []
        cache_bytes_rows = []
        for stage in STAGES:
            obj_row = [stage]
            bytes_row = [stage]
            for label in labels_in_order:
                r = by_key.get((label, phase_key, stage))
                obj_row.append(format_count(int(r["cache_object_count"])) if r else "-")
                bytes_row.append(format_bytes(float(r["cache_bytes"]) if r else 0) if r else "-")
            cache_obj_rows.append(obj_row)
            cache_bytes_rows.append(bytes_row)
        sections.append("### Cache objects\n")
        sections.append(markdown_table(["Stage", *labels_in_order], cache_obj_rows))
        sections.append("")
        sections.append("### Cache bytes (approximate)\n")
        sections.append(markdown_table(["Stage", *labels_in_order], cache_bytes_rows))
        sections.append("")

        # Process memory: one row per test case (not per stage - RSS/
        # swap are whole-process, so the peak across this phase's own 5
        # stage-compute samples is the meaningful number, not a
        # per-stage breakdown that would just repeat the same running
        # total 5 times).
        mem_rows = []
        for label in labels_in_order:
            peak_rss = max(
                (float(by_key[(label, phase_key, s)]["rss_after_mb"]) for s in STAGES if (label, phase_key, s) in by_key),
                default=0.0,
            )
            peak_swap = max(
                (float(by_key[(label, phase_key, s)]["swap_after_mb"]) for s in STAGES if (label, phase_key, s) in by_key),
                default=0.0,
            )
            mem_rows.append([label, f"{peak_rss:.1f} MB", f"{peak_swap:.1f} MB" if peak_swap > 0 else "0"])
        sections.append("### Process memory\n")
        sections.append(markdown_table(["Test case", "Peak RSS", "Peak swap"], mem_rows))
        sections.append("")

    return "\n".join(sections)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--name", required=True, help="Prefix for the generated <name>.csv/<name>.md files")
    script_dir = Path(__file__).resolve().parent
    parser.add_argument(
        "--bin",
        default=str(script_dir.parent / "build_release" / "pipeline_stage_benchmark"),
        help="Path to the compiled pipeline_stage_benchmark tool (default: ../build_release/pipeline_stage_benchmark)",
    )
    parser.add_argument(
        "--test-data-dir",
        default=str(script_dir.parent.parent / "test_data"),
        help="Path to the repo's test_data/ directory (default: ../../test_data)",
    )
    parser.add_argument(
        "--output-dir",
        default="benchmark_results",
        help="Directory to write <name>.csv/<name>.md into - created if it doesn't exist (default: benchmark_results)",
    )
    args = parser.parse_args()

    binary = Path(args.bin).resolve()
    if not binary.exists():
        raise SystemExit(f"pipeline_stage_benchmark binary not found at {binary} - build it first (see CMakeLists.txt)")

    test_data_dir = Path(args.test_data_dir).resolve()
    cases = build_test_cases(test_data_dir)

    all_rows: list[dict] = []
    for case in cases:
        all_rows.extend(run_case(binary, case))

    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    csv_path = output_dir / f"{args.name}.csv"
    md_path = output_dir / f"{args.name}.md"

    write_csv(all_rows, csv_path)
    labels_in_order = [c["label"] for c in cases]
    md_path.write_text(generate_markdown(all_rows, labels_in_order))

    print(f"Wrote {csv_path} ({len(all_rows)} rows) and {md_path}", file=sys.stderr)


if __name__ == "__main__":
    main()
