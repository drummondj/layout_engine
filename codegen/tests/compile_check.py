"""Compiles generated headers, when a C++23 compiler is available."""

import os
import shutil
import subprocess
from pathlib import Path
from typing import Optional


def compiler() -> Optional[str]:
    """$CXX (ctest sets it to the build's compiler), else c++ on PATH."""
    return os.environ.get("CXX") or shutil.which("c++")


def compile_headers(directory: str) -> subprocess.CompletedProcess:
    """Syntax-checks one translation unit including every header in `directory`."""
    headers = sorted(p.name for p in Path(directory).glob("*.hpp"))
    source = "".join(f'#include "{name}"\n' for name in headers)
    return subprocess.run(
        [compiler(), "-std=c++23", "-fsyntax-only", "-x", "c++", "-I", directory, "-"],
        input=source,
        capture_output=True,
        text=True,
    )
