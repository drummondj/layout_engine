#!/usr/bin/env python3
"""
Fails when test code names a file in the shared temp directory that isn't
unique to its process. ctest runs every discovered GTest, and every Tcl
test, as its own process in parallel, and other build trees (a Release
tree, an `le` project) may run the same tests at the same time: a fixed
name like /tmp/le_roundtrip.lef is overwritten and read by several tests at
once, and they fail intermittently.

Rules, for test sources (any tests/ directory, and src/test_support/):
- C++: a line using temp_directory_path() or tmpnam must also use getpid.
  Core tests use le::test::scratch_path (src/test_support/scratch_dir.hpp).
- Tcl: a line using TMPDIR or /tmp must also use [pid], or use `file tempfile`.
- Python: no /tmp literals; use tempfile.

    python3 scripts/check_test_scratch_paths.py [<repo root>]
"""

import re
import sys
from pathlib import Path
from typing import List

ROOTS = ("src", "examples", "tools", "codegen", "cmake")
SKIP_PARTS = {"_deps", "lefdef", "__pycache__", "build", "generated"}


def is_test_source(path: Path, root: Path) -> bool:
    parts = path.relative_to(root).parts
    if SKIP_PARTS & set(parts):
        return False
    return "tests" in parts or parts[:2] == ("src", "test_support")


def problems(path: Path, text: str) -> List[str]:
    """Each offending line of one test source, as "path:line: why"."""
    found = []
    suffix = path.suffix
    for number, line in enumerate(text.splitlines(), 1):
        code = line.split("//", 1)[0] if suffix in (".cpp", ".hpp", ".h") else line
        stripped = code.strip()
        if suffix in (".cpp", ".hpp", ".h"):
            if re.search(r"temp_directory_path|\btmpnam\b", code) and "getpid" not in code:
                found.append(f"{path}:{number}: a temp file without the pid - use le::test::scratch_path (or add getpid)")
        elif suffix == ".tcl":
            if stripped.startswith("#"):
                continue
            if re.search(r"TMPDIR|/tmp\b", code) and "[pid]" not in code:
                found.append(f"{path}:{number}: a temp path without [pid] - add [pid] or use `file tempfile`")
        elif suffix == ".py":
            if stripped.startswith("#"):
                continue
            if re.search(r"""["']/tmp\b""", code):
                found.append(f"{path}:{number}: a /tmp literal - use tempfile")
    return found


def check(root: Path) -> List[str]:
    found = []
    for top in ROOTS:
        for path in sorted((root / top).rglob("*")):
            if path.is_file() and path.suffix in (".cpp", ".hpp", ".h", ".tcl", ".py") and is_test_source(path, root):
                if path.resolve() == Path(__file__).resolve():
                    continue
                found += problems(path.relative_to(root), path.read_text(errors="replace"))
    return found


def main() -> int:
    root = Path(sys.argv[1] if len(sys.argv) > 1 else Path(__file__).resolve().parents[1]).resolve()
    found = check(root)
    for line in found:
        print(line)
    if found:
        print(f"{len(found)} test scratch path(s) shared between parallel test processes - see this script's docstring")
        return 1
    print("test scratch paths are unique per process")
    return 0


if __name__ == "__main__":
    sys.exit(main())
