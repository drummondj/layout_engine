"""
Reads Layout Engine extension manifests (le_extension.toml), checks them
against the running build and each other, orders them by dependency, and
writes a CMake include describing them - run by CMakeLists.txt at configure
time for every directory in LE_EXTENSION_DIRS. The manifest format is
documented in docs/EXTENSION_MECHANISM_RESEARCH.md §2.

Standard library only (tomllib needs Python 3.11), so the package manager can
reuse it without codegen's other dependencies.
"""

import argparse
import re
import sys
import tomllib
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Optional, Tuple

MANIFEST_NAME = "le_extension.toml"

_NAME = re.compile(r"^[a-z][a-z0-9_]*$")
_PREFIX = re.compile(r"^[A-Z][A-Za-z0-9]*$")
_VERSION = re.compile(r"^(\d+)\.(\d+)\.(\d+)$")
_CONSTRAINT = re.compile(r"^(>=|<=|==|!=|>|<)?\s*(\d+(?:\.\d+){0,2})$")


class ManifestError(Exception):
    pass


def parse_version(text: str) -> Tuple[int, int, int]:
    match = _VERSION.match(text)
    if not match:
        raise ManifestError(f"version {text!r} is not MAJOR.MINOR.PATCH")
    return (int(match.group(1)), int(match.group(2)), int(match.group(3)))


def satisfies(version: str, spec: str) -> bool:
    """
    Whether `version` (MAJOR.MINOR.PATCH) meets `spec`: comma-separated
    constraints such as ">=0.9, <0.11". A constraint's version may omit
    trailing parts (">=0.9" means >=0.9.0); a bare version means ==.
    """
    actual = parse_version(version)
    for part in spec.split(","):
        part = part.strip()
        match = _CONSTRAINT.match(part)
        if not match:
            raise ManifestError(f"version constraint {part!r} not understood (in {spec!r})")
        op = match.group(1) or "=="
        numbers = [int(n) for n in match.group(2).split(".")]
        wanted = tuple(numbers + [0] * (3 - len(numbers)))
        ok = {
            ">=": actual >= wanted,
            "<=": actual <= wanted,
            ">": actual > wanted,
            "<": actual < wanted,
            "==": actual == wanted,
            "!=": actual != wanted,
        }[op]
        if not ok:
            return False
    return True


def check_spec(spec: str) -> None:
    """Raises ManifestError unless `spec` is a well-formed constraint list."""
    satisfies("0.0.0", spec)


@dataclass
class Manifest:
    directory: Path
    name: str
    version: str
    prefix: str
    description: str = ""
    layout_engine: str = ""
    extension_api: int = 0
    dependencies: Dict[str, str] = field(default_factory=dict)
    cmake: Optional[Path] = None
    schema: Optional[Path] = None
    migrations: Optional[Path] = None
    tcl_procs: List[Path] = field(default_factory=list)
    resources: List[Path] = field(default_factory=list)

    @property
    def compiled(self) -> bool:
        """Whether the extension builds C++ (the compiled tier, not script-only)."""
        return self.cmake is not None or self.schema is not None


def _table(data: dict, key: str, where: str) -> dict:
    value = data.get(key, {})
    if not isinstance(value, dict):
        raise ManifestError(f"{where}: [{key}] must be a table")
    return value


def _string(table: dict, key: str, where: str, required: bool = True) -> str:
    value = table.get(key)
    if value is None:
        if required:
            raise ManifestError(f"{where}: missing {key}")
        return ""
    if not isinstance(value, str):
        raise ManifestError(f"{where}: {key} must be a string")
    return value


def _path(directory: Path, value, key: str, where: str) -> Path:
    if not isinstance(value, str):
        raise ManifestError(f"{where}: {key} must be a path string")
    path = (directory / value).resolve()
    if not path.exists():
        raise ManifestError(f"{where}: {key} {value!r} does not exist")
    if directory.resolve() not in path.parents and path != directory.resolve():
        raise ManifestError(f"{where}: {key} {value!r} is outside the extension directory")
    return path


def load(directory: Path) -> Manifest:
    """Reads and checks one extension directory's manifest on its own."""
    directory = Path(directory)
    manifest_path = directory / MANIFEST_NAME
    where = str(manifest_path)
    if not manifest_path.is_file():
        raise ManifestError(f"{directory}: no {MANIFEST_NAME}")
    try:
        data = tomllib.loads(manifest_path.read_text())
    except tomllib.TOMLDecodeError as e:
        raise ManifestError(f"{where}: {e}") from e

    extension = _table(data, "extension", where)
    compatibility = _table(data, "compatibility", where)
    dependencies = _table(data, "dependencies", where)
    contents = _table(data, "contents", where)

    name = _string(extension, "name", where)
    if not _NAME.match(name):
        raise ManifestError(f"{where}: name {name!r} must be snake_case (lower case, digits, underscores)")
    version = _string(extension, "version", where)
    parse_version(version)
    prefix = _string(extension, "prefix", where)
    if not _PREFIX.match(prefix):
        raise ManifestError(f"{where}: prefix {prefix!r} must be PascalCase, e.g. Acme")

    layout_engine = _string(compatibility, "layout_engine", where)
    if layout_engine:
        check_spec(layout_engine)
    extension_api = compatibility.get("extension_api")
    if not isinstance(extension_api, int) or isinstance(extension_api, bool):
        raise ManifestError(f"{where}: [compatibility] extension_api must be an integer")

    for dependency, spec in dependencies.items():
        if not isinstance(spec, str):
            raise ManifestError(f"{where}: dependency {dependency} must be a version constraint string")
        check_spec(spec)

    manifest = Manifest(
        directory=directory.resolve(),
        name=name,
        version=version,
        prefix=prefix,
        description=_string(extension, "description", where, required=False),
        layout_engine=layout_engine,
        extension_api=extension_api,
        dependencies=dict(dependencies),
    )
    for key in ("cmake", "schema", "migrations"):
        if key in contents:
            setattr(manifest, key, _path(directory, contents[key], key, where))
    for key in ("tcl_procs", "resources"):
        values = contents.get(key, [])
        if not isinstance(values, list):
            raise ManifestError(f"{where}: {key} must be a list of paths")
        setattr(manifest, key, [_path(directory, v, key, where) for v in values])
    unknown = set(contents) - {"cmake", "schema", "migrations", "tcl_procs", "resources"}
    if unknown:
        raise ManifestError(f"{where}: unknown [contents] keys {sorted(unknown)}")
    return manifest


def check_and_order(manifests: List[Manifest], layout_engine_version: str, extension_api: int) -> List[Manifest]:
    """
    Checks the set of extensions against this build and each other, and
    returns them in dependency order (a dependency before its dependents,
    ties broken by name). Reports every problem at once.
    """
    problems: List[str] = []
    by_name: Dict[str, Manifest] = {}
    prefixes: Dict[str, str] = {}
    for m in manifests:
        if m.name in by_name:
            problems.append(f"extension {m.name} is listed twice ({by_name[m.name].directory} and {m.directory})")
            continue
        by_name[m.name] = m
        if m.prefix in prefixes:
            problems.append(f"extensions {prefixes[m.prefix]} and {m.name} share the prefix {m.prefix}")
        prefixes[m.prefix] = m.name
        if m.extension_api != extension_api:
            problems.append(f"{m.name} {m.version} targets extension API {m.extension_api}; this build provides {extension_api}")
        if m.layout_engine and not satisfies(layout_engine_version, m.layout_engine):
            problems.append(f"{m.name} {m.version} needs layout_engine {m.layout_engine}; this build is {layout_engine_version}")
    for m in by_name.values():
        for dependency, spec in m.dependencies.items():
            other = by_name.get(dependency)
            if other is None:
                problems.append(f"{m.name} depends on {dependency}, which isn't in LE_EXTENSION_DIRS")
            elif not satisfies(other.version, spec):
                problems.append(f"{m.name} needs {dependency} {spec}; {other.version} is listed")
    if problems:
        raise ManifestError("\n".join(problems))

    # Kahn's algorithm, always taking the alphabetically first extension
    # whose dependencies are all placed: name order wherever dependencies
    # allow it.
    remaining = {name: set(m.dependencies) for name, m in by_name.items()}
    ordered: List[Manifest] = []
    while remaining:
        ready = sorted(name for name, deps in remaining.items() if not deps)
        if not ready:
            raise ManifestError("extension dependency cycle among: " + ", ".join(sorted(remaining)))
        name = ready[0]
        ordered.append(by_name[name])
        del remaining[name]
        for deps in remaining.values():
            deps.discard(name)
    return ordered


def _cmake_string(value: str) -> str:
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"').replace("$", "\\$") + '"'


def _cmake_list(paths: List[Path]) -> str:
    return _cmake_string(";".join(str(p) for p in paths))


def to_cmake(ordered: List[Manifest]) -> str:
    """The CMake include: LE_EXTENSIONS (ordered names) and LE_EXTENSION_<name>_* per extension."""
    lines = [
        "# Generated by codegen.extension_manifest from LE_EXTENSION_DIRS - do not edit.",
        f"set(LE_EXTENSIONS {_cmake_string(';'.join(m.name for m in ordered))})",
    ]
    for m in ordered:
        p = f"LE_EXTENSION_{m.name}"
        lines += [
            f"set({p}_DIR {_cmake_string(str(m.directory))})",
            f"set({p}_MANIFEST {_cmake_string(str(m.directory / MANIFEST_NAME))})",
            f"set({p}_VERSION {_cmake_string(m.version)})",
            f"set({p}_PREFIX {_cmake_string(m.prefix)})",
            f"set({p}_COMPILED {'ON' if m.compiled else 'OFF'})",
            f"set({p}_CMAKE {_cmake_string(str(m.cmake) if m.cmake else '')})",
            f"set({p}_SCHEMA {_cmake_string(str(m.schema) if m.schema else '')})",
            f"set({p}_TCL_PROCS {_cmake_list(m.tcl_procs)})",
            f"set({p}_RESOURCES {_cmake_list(m.resources)})",
        ]
    return "\n".join(lines) + "\n"


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.strip().splitlines()[0])
    parser.add_argument("--layout-engine-version", required=True)
    parser.add_argument("--extension-api", required=True, type=int)
    parser.add_argument("--output", required=True, help="CMake include to write")
    parser.add_argument("directories", nargs="*", help="extension directories (LE_EXTENSION_DIRS)")
    args = parser.parse_args(argv)
    try:
        manifests = [load(Path(d)) for d in args.directories]
        ordered = check_and_order(manifests, args.layout_engine_version, args.extension_api)
    except ManifestError as e:
        sys.stderr.write(f"extension manifest error:\n{e}\n")
        return 1
    output = Path(args.output)
    text = to_cmake(ordered)
    if not output.exists() or output.read_text() != text:
        output.write_text(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
