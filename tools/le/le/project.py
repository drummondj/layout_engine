"""
le_project.toml: what a project wants. Read with tomllib; `add`, `remove` and
`trust` edit it line by line, so comments and layout survive.

    [project]
    name = "my_chip"
    startup = "init.tcl"                      # optional

    [layout_engine]
    github = "drummondj/layout_engine"       # with tag, rev or version
    version = ">=0.3, <0.4"
    # path = "../layout_engine"              # or a local checkout

    [build]
    type = "Release"                          # optional
    jobs = 8                                  # optional
    cmake_args = ["-DLE_ENABLE_TRACY=OFF"]    # optional: passed to cmake when configuring

    [extensions]
    acme = { github = "acme/acme", version = ">=1.0, <2", publisher = "acme" }
    mine = { path = "../mine" }

    [trust]
    acme = ["ssh-ed25519 AAAA... release@acme.com"]
"""

import re
import tomllib
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Optional

PROJECT_FILE = "le_project.toml"

_NAME = re.compile(r"^[a-z][a-z0-9_]*$")
_REPO = re.compile(r"^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$")


class ProjectError(Exception):
    pass


@dataclass
class Source:
    """Where something comes from: a GitHub repo at a tag or revision, or a local path."""

    github: Optional[str] = None
    tag: Optional[str] = None
    rev: Optional[str] = None
    version: Optional[str] = None  # a constraint, resolved against vX.Y.Z tags
    path: Optional[Path] = None
    publisher: Optional[str] = None
    allow_unsigned: bool = False

    def describe(self) -> str:
        if self.path is not None:
            return f"path:{self.path}"
        return f"github:{self.github}@{self.tag or self.rev or self.version}"


@dataclass
class Project:
    root: Path
    name: str
    startup: Optional[Path]
    layout_engine: Source
    build_type: str
    jobs: Optional[int]
    extensions: Dict[str, Source] = field(default_factory=dict)
    cmake_args: List[str] = field(default_factory=list)
    trust: Dict[str, List[str]] = field(default_factory=dict)

    @property
    def file(self) -> Path:
        return self.root / PROJECT_FILE


def _source(table: dict, where: str, root: Path, is_extension: bool) -> Source:
    if not isinstance(table, dict):
        raise ProjectError(f"{where} must be a table")
    known = {"github", "tag", "rev", "version", "path", "publisher", "allow_unsigned"}
    unknown = set(table) - known
    if unknown:
        raise ProjectError(f"{where}: unknown keys {sorted(unknown)}")
    source = Source(
        github=table.get("github"),
        tag=table.get("tag"),
        rev=table.get("rev"),
        version=table.get("version"),
        path=(root / table["path"]).resolve() if "path" in table else None,
        publisher=table.get("publisher"),
        allow_unsigned=bool(table.get("allow_unsigned", False)),
    )
    if (source.github is None) == (source.path is None):
        raise ProjectError(f"{where}: give exactly one of github or path")
    if source.github is not None:
        if not _REPO.match(source.github):
            raise ProjectError(f"{where}: github must be owner/repo, got {source.github!r}")
        if sum(v is not None for v in (source.tag, source.rev, source.version)) != 1:
            raise ProjectError(f"{where}: a github source needs exactly one of tag, rev or version")
        if is_extension and source.publisher is None and not source.allow_unsigned:
            raise ProjectError(f"{where}: a github extension needs a publisher (whose key in [trust] signs it)")
    elif source.tag or source.rev or source.version or source.publisher:
        raise ProjectError(f"{where}: a path source takes no tag, rev, version or publisher")
    return source


def load(root: Path) -> Project:
    root = Path(root).resolve()
    path = root / PROJECT_FILE
    if not path.is_file():
        raise ProjectError(f"no {PROJECT_FILE} in {root} - run `le init` there first")
    try:
        data = tomllib.loads(path.read_text())
    except tomllib.TOMLDecodeError as e:
        raise ProjectError(f"{path}: {e}") from e

    project = data.get("project", {})
    if "name" not in project:
        raise ProjectError(f"{path}: [project] needs a name")
    if "layout_engine" not in data:
        raise ProjectError(f"{path}: missing [layout_engine]")
    build = data.get("build", {})
    jobs = build.get("jobs")
    if jobs is not None and (not isinstance(jobs, int) or jobs < 1):
        raise ProjectError(f"{path}: [build] jobs must be a positive integer")
    cmake_args = build.get("cmake_args", [])
    if not isinstance(cmake_args, list) or not all(isinstance(a, str) for a in cmake_args):
        raise ProjectError(f"{path}: [build] cmake_args must be a list of strings")
    reserved = [a for a in cmake_args if a.split("=", 1)[0] in ("-DLE_EXTENSION_DIRS", "-DCMAKE_BUILD_TYPE", "-S", "-B")]
    if reserved:
        raise ProjectError(f"{path}: [build] cmake_args can't set {', '.join(reserved)} - le does ([extensions], [build] type)")

    extensions = {}
    for name, table in data.get("extensions", {}).items():
        if not _NAME.match(name):
            raise ProjectError(f"{path}: extension name {name!r} must be snake_case")
        extensions[name] = _source(table, f"{path}: extension {name}", root, True)
    trust = {}
    for publisher, keys in data.get("trust", {}).items():
        if not isinstance(keys, list) or not all(isinstance(k, str) and k.startswith("ssh-") for k in keys):
            raise ProjectError(f"{path}: [trust] {publisher} must be a list of SSH public keys")
        trust[publisher] = keys
    for name, source in extensions.items():
        if source.publisher is not None and source.publisher not in trust:
            raise ProjectError(f"{path}: extension {name}'s publisher {source.publisher} has no keys in [trust] (`le trust`)")

    return Project(
        root=root,
        name=project["name"],
        startup=(root / project["startup"]).resolve() if "startup" in project else None,
        layout_engine=_source(data["layout_engine"], f"{path}: [layout_engine]", root, False),
        build_type=build.get("type", "Release"),
        jobs=jobs,
        cmake_args=cmake_args,
        extensions=extensions,
        trust=trust,
    )


# --- Line-based edits ----------------------------------------------------------


def _quote(value: str) -> str:
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'


def inline_table(entries: Dict[str, object]) -> str:
    parts = []
    for key, value in entries.items():
        if isinstance(value, bool):
            parts.append(f"{key} = {'true' if value else 'false'}")
        else:
            parts.append(f"{key} = {_quote(str(value))}")
    return "{ " + ", ".join(parts) + " }"


def _section_bounds(lines: List[str], section: str):
    """(index of the [section] header, index just past its last non-blank line), or None."""
    header = f"[{section}]"
    start = next((i for i, line in enumerate(lines) if line.strip() == header), None)
    if start is None:
        return None
    end = start + 1
    for i in range(start + 1, len(lines)):
        stripped = lines[i].strip()
        if stripped.startswith("[") and not stripped.startswith("[["):
            break
        if stripped:
            end = i + 1
    return start, end


def set_entry(text: str, section: str, key: str, value: str) -> str:
    """Sets `key = value` in [section] (replacing an existing entry, else appending one)."""
    lines = text.splitlines()
    pattern = re.compile(rf"^\s*{re.escape(key)}\s*=")
    bounds = _section_bounds(lines, section)
    if bounds is None:
        if lines and lines[-1].strip():
            lines.append("")
        lines += [f"[{section}]", f"{key} = {value}"]
    else:
        start, end = bounds
        for i in range(start + 1, end):
            if pattern.match(lines[i]):
                lines[i] = f"{key} = {value}"
                break
        else:
            lines.insert(end, f"{key} = {value}")
    return "\n".join(lines) + "\n"


def remove_entry(text: str, section: str, key: str) -> str:
    lines = text.splitlines()
    bounds = _section_bounds(lines, section)
    pattern = re.compile(rf"^\s*{re.escape(key)}\s*=")
    if bounds is None:
        raise ProjectError(f"no [{section}] section")
    start, end = bounds
    for i in range(start + 1, end):
        if pattern.match(lines[i]):
            del lines[i]
            return "\n".join(lines) + "\n"
    raise ProjectError(f"{key} isn't in [{section}]")


def template(name: str, layout_engine: Dict[str, object]) -> str:
    return f"""# A Layout Engine project: `le install` builds Layout Engine with these
# extensions into .le/, and `le shell` runs it. See plans/PACKAGE_MANAGER_RESEARCH.md.

[project]
name = {_quote(name)}
# startup = "init.tcl"      # sourced after every extension's procs

[layout_engine]
{chr(10).join(f"{k} = {_quote(str(v))}" for k, v in layout_engine.items())}

[build]
type = "Release"
# cmake_args = ["-DLE_ENABLE_TRACY=OFF"]    # passed to cmake when configuring

[extensions]

[trust]
"""
