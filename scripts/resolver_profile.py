#!/usr/bin/env python3
"""Runs build_release/resolver_profile once per aes_scaling design (one
process each, so every column's peak RSS is that design's alone) and prints
a Markdown table: one row per metric, one column per design.

    scripts/resolver_profile.py [--repeats N] [--binary PATH] LABEL [LABEL ...]

e.g. scripts/resolver_profile.py 1x1 2x1 2x2 3x2 3x3 4x4 5x5
"""

import argparse
import pathlib
import subprocess
import sys


def run(binary: pathlib.Path, label: str, repeats: int) -> dict[str, str]:
    result = subprocess.run([str(binary), label, str(repeats)], capture_output=True, text=True)
    if result.returncode != 0:
        sys.stderr.write(f"{label}: exit {result.returncode}\n{result.stderr[-2000:]}\n")
        return {}
    metrics: dict[str, str] = {}
    for line in result.stdout.splitlines():
        parts = line.split()
        if len(parts) == 2 and not line.startswith("["):
            metrics[parts[0]] = parts[1]
    return metrics


def fmt(value: str | None) -> str:
    if value is None:
        return "-"
    try:
        number = float(value)
    except ValueError:
        return value
    if number >= 100 or number == int(number):
        return f"{number:,.0f}"
    return f"{number:.2f}"


def main() -> int:
    backend = pathlib.Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("labels", nargs="+")
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--binary", type=pathlib.Path, default=backend / "build_release" / "resolver_profile")
    args = parser.parse_args()

    results: dict[str, dict[str, str]] = {}
    order: list[str] = []
    for label in args.labels:
        sys.stderr.write(f"profiling {label}...\n")
        metrics = run(args.binary, label, args.repeats)
        results[label] = metrics
        for name in metrics:
            if name not in order:
                order.append(name)

    print("| Metric | " + " | ".join(args.labels) + " |")
    print("| --- | " + " | ".join("---:" for _ in args.labels) + " |")
    for name in order:
        if name == "design":
            continue
        print(f"| {name} | " + " | ".join(fmt(results[label].get(name)) for label in args.labels) + " |")
    return 0


if __name__ == "__main__":
    sys.exit(main())
