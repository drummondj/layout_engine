"""
codegen's Python for a source build: a private venv in le's cache holding
the packages codegen needs (the dependencies in <layout_engine>/codegen/
pyproject.toml), shared by every project with the same requirements and base
Python. `le` passes it to CMake as Python3_EXECUTABLE, so a build never needs
`pip install ./codegen` into a system Python. The build runs codegen itself
from the Layout Engine source tree, so the venv holds only its dependencies.

A project that sets -DPython3_EXECUTABLE in [build] cmake_args uses that
interpreter instead. If the venv can't be made (no network, no venv module),
a base Python that already has the packages is used as it is.
"""

import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
import tomllib
from pathlib import Path
from typing import List, Optional, Tuple

from le import sources

MINIMUM = (3, 11)
BELOW = (3, 14)  # codegen's pyproject.toml: python = ">=3.11,<3.14"
READY = "le_ready"  # written into a venv once its packages are installed

_FROZEN = getattr(sys, "frozen", False)  # running as the PyInstaller binary


class PythonEnvError(Exception):
    pass


def requirements(le_source: Path) -> Optional[List[str]]:
    """codegen's dependencies as pip requirements, sorted; None for a source tree without codegen/pyproject.toml."""
    pyproject = le_source / "codegen" / "pyproject.toml"
    if not pyproject.is_file():
        return None
    dependencies = tomllib.loads(pyproject.read_text()).get("tool", {}).get("poetry", {}).get("dependencies", {})
    return sorted(pip_requirement(name, spec) for name, spec in dependencies.items() if name != "python")


def pip_requirement(name: str, spec) -> str:
    """A Poetry version constraint as a pip requirement: ^ and ~ become ranges, a bare version is exact."""
    if isinstance(spec, dict):
        spec = spec.get("version", "*")
    spec = spec.strip()
    if spec in ("", "*"):
        return name
    if spec[0] in "^~" and not spec.startswith("~="):
        base = spec[1:].strip()
        parts = [int(p) for p in base.split(".")]
        if spec[0] == "^":
            # The leftmost non-zero part may not change: ^1.2.3 is <2.0.0, ^0.2.3 is <0.3.0.
            position = next((i for i, p in enumerate(parts) if p != 0), len(parts) - 1)
        else:
            # ~1.2.3 is <1.3.0; ~1 is <2.
            position = 0 if len(parts) == 1 else 1
        upper = parts[:position] + [parts[position] + 1]
        return f"{name}>={base},<{'.'.join(map(str, upper))}"
    if spec[0].isdigit():
        return f"{name}=={spec}"
    return f"{name}{spec.replace(' ', '')}"


def _version(python: str) -> Optional[Tuple[int, int]]:
    try:
        result = subprocess.run([python, "-c", "import sys; print('%d.%d' % sys.version_info[:2])"], capture_output=True, text=True, timeout=30)
    except (OSError, subprocess.TimeoutExpired):
        return None
    if result.returncode != 0:
        return None
    major, minor = result.stdout.strip().split(".")
    return int(major), int(minor)


def base_python() -> Tuple[str, Tuple[int, int]]:
    """(interpreter, (major, minor)) to make the venv from: the one running `le`
    (unless it's the frozen binary), else the newest suitable python3.x on PATH."""
    candidates = [] if _FROZEN else [sys.executable]
    candidates += [shutil.which(f"python3.{minor}") for minor in range(BELOW[1] - 1, MINIMUM[1] - 1, -1)]
    candidates.append(shutil.which("python3"))
    for python in candidates:
        if python is None:
            continue
        version = _version(python)
        if version is not None and MINIMUM <= version < BELOW:
            return python, version
    raise PythonEnvError(
        f"codegen needs Python >={MINIMUM[0]}.{MINIMUM[1]},<{BELOW[0]}.{BELOW[1]} and there's none on PATH. Install one, "
        "or point [build] cmake_args at one that has codegen's packages: cmake_args = [\"-DPython3_EXECUTABLE=/path/to/python3\"]"
    )


def env_dir(reqs: List[str], version: Tuple[int, int]) -> Path:
    """The venv for these requirements on this Python version, under <cache>/python/."""
    identity = "\n".join([f"{version[0]}.{version[1]}", os.uname().machine, *reqs])
    key = hashlib.sha256(identity.encode()).hexdigest()[:16]
    return sources.cache_dir() / "python" / key


def has_packages(python: str, reqs: List[str]) -> bool:
    """Whether `python` already has a distribution for every requirement (versions aren't checked)."""
    names = [_name(r) for r in reqs]
    check = "import importlib.metadata as m, sys\nfor n in sys.argv[1:]: m.distribution(n)\n"
    try:
        return subprocess.run([python, "-c", check, *names], capture_output=True, timeout=60).returncode == 0
    except (OSError, subprocess.TimeoutExpired):
        return False


def _name(requirement: str) -> str:
    for i, c in enumerate(requirement):
        if c in "<>=!~;[ ":
            return requirement[:i]
    return requirement


def _create(base: str, directory: Path, reqs: List[str]) -> None:
    """Makes the venv in a staging directory beside `directory` and renames it
    into place whole, so a concurrent `le` never uses half of one; the first
    to finish wins."""
    directory.parent.mkdir(parents=True, exist_ok=True)
    staging = Path(tempfile.mkdtemp(prefix=f".{directory.name}-", dir=directory.parent))
    try:
        venv = staging / "venv"
        for command, what in (
            ([base, "-m", "venv", str(venv)], "creating the venv"),
            ([str(venv / "bin" / "python"), "-m", "pip", "install", "--quiet", "--disable-pip-version-check", *reqs], "installing " + " ".join(reqs)),
        ):
            result = subprocess.run(command, capture_output=True, text=True)
            if result.returncode != 0:
                raise PythonEnvError(f"{what} failed:\n{(result.stdout + result.stderr).strip()}")
        (venv / READY).write_text(" ".join(reqs) + "\n")
        try:
            venv.rename(directory)
        except OSError:
            if not (directory / READY).is_file():
                raise
    finally:
        shutil.rmtree(staging, ignore_errors=True)


def codegen_python(le_source: Path, cmake_args: Optional[List[str]] = None) -> Optional[Path]:
    """The Python to configure `le_source` with, making its venv if needed;
    None when the project chose its own (-DPython3_EXECUTABLE in cmake_args)
    or the source tree doesn't say what codegen needs."""
    if any(a.split("=", 1)[0].split(":", 1)[0] == "-DPython3_EXECUTABLE" for a in cmake_args or []):
        return None
    reqs = requirements(le_source)
    if reqs is None:
        return None
    base, version = base_python()
    directory = env_dir(reqs, version)
    python = directory / "bin" / "python"
    if (directory / READY).is_file() and python.exists():
        return python
    if directory.exists():
        shutil.rmtree(directory, ignore_errors=True)  # an unfinished or broken one
    print(f"le: making codegen's Python environment ({' '.join(reqs)}) in {directory}", flush=True)
    try:
        _create(base, directory, reqs)
    except PythonEnvError as e:
        if has_packages(base, reqs):
            print(f"le: warning: couldn't make codegen's Python environment; using {base}, which has its packages already\n{e}",
                  file=sys.stderr, flush=True)
            return Path(base)
        raise PythonEnvError(
            f"couldn't make codegen's Python environment from {base} ({e}).\n"
            "Without network access, or without Python's venv module (Debian/Ubuntu: the python3-venv package), install "
            f"{' '.join(reqs)} for a Python >={MINIMUM[0]}.{MINIMUM[1]} yourself and point the project at it: "
            "[build] cmake_args = [\"-DPython3_EXECUTABLE=/path/to/python3\"]"
        ) from e
    return python
