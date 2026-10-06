"""
`le install`: resolve the project's sources (or replay the lock), check
signatures and manifests, write the lock, and build the bundle.

Resolution happens when there's no lock, le_project.toml changed since it
was written, or `le update` asks for it. Otherwise the lock is replayed: each
GitHub source must still resolve to its locked commit, signed by its locked
key, so a moved tag or a swapped key stops the install.
"""

import json
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Set

from le import build, lock as lockfile, project as projectfile, sources

_REPO_ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(_REPO_ROOT / "codegen"))
from codegen import extension_manifest  # noqa: E402

LAYOUT_ENGINE_PRINCIPAL = "release@layout-engine"
STATE_DIR = ".le"


class InstallError(Exception):
    pass


def layout_engine_allowed_signers() -> Path:
    """The release key `le` trusts for Layout Engine's own tags."""
    path = _REPO_ROOT / "assets" / "keys" / "allowed_signers"
    if not path.is_file():
        raise InstallError(f"Layout Engine's release key isn't at {path}")
    return path


@dataclass
class Fetched:
    directory: Path
    locked: lockfile.LockedSource


def _fetch(
    state: Path,
    name: str,
    source: projectfile.Source,
    allowed_signers: Optional[Path],
    principal: Optional[str],
    locked: Optional[lockfile.LockedSource],
) -> Fetched:
    """Makes `source` available under .le/src/<name> (or in place, for a path)."""
    if source.path is not None:
        if not source.path.is_dir():
            raise InstallError(f"{name}: {source.path} isn't a directory")
        return Fetched(source.path, lockfile.LockedSource(source=f"path:{source.path}"))

    repo_dir = sources.mirror(source.github)
    commit, ref, is_tag = sources.resolve(repo_dir, source.tag, source.rev)
    signer = None
    if not source.allow_unsigned:
        signer = sources.verify(repo_dir, ref, is_tag, allowed_signers, principal)
    if locked is not None:
        if locked.rev != commit:
            raise InstallError(
                f"{name}: {source.describe()} is now commit {commit[:12]}, but the lock has {locked.rev[:12]} - "
                "the tag moved. Run `le update` to accept it after checking why."
            )
        if locked.signer != signer:
            raise InstallError(f"{name}: signed by key {signer}, but the lock has {locked.signer} - run `le update` to accept a new key")
    if source.allow_unsigned:
        print(f"le: warning: {name} is installed unsigned ({source.describe()})", file=sys.stderr)
    directory = state / "src" / name
    sources.export(repo_dir, commit, directory)
    return Fetched(directory, lockfile.LockedSource(source=f"github:{source.github}", rev=commit, signer=signer, unsigned=source.allow_unsigned))


def _publisher_signers(state: Path, project: projectfile.Project, publisher: str) -> Path:
    return sources.write_allowed_signers(state / "trust" / f"{publisher}.allowed_signers", publisher, project.trust[publisher])


def install(root: Path, update: Optional[Set[str]] = None) -> Path:
    """
    Installs the project at `root` into .le/bundle; returns the bundle.
    `update` names entries to re-resolve ignoring the lock ("layout_engine"
    or extension names); an empty set means all of them.
    """
    project = projectfile.load(root)
    state = project.root / STATE_DIR
    state.mkdir(exist_ok=True)
    current_hash = lockfile.project_hash(project.file)
    old = lockfile.load(project.root)

    # The lock pins everything until le_project.toml changes (then all of it
    # is resolved again) or `le update` names an entry (or, with no names,
    # every entry).
    def locked_for(name: str) -> Optional[lockfile.LockedSource]:
        if old is None or old.project_hash != current_hash:
            return None
        if update is not None and (not update or name in update):
            return None
        if name == "layout_engine":
            return old.layout_engine
        entry = old.extension(name)
        return entry.locked if entry is not None else None

    le_fetched = _fetch(
        state, "layout_engine", project.layout_engine, layout_engine_allowed_signers(), LAYOUT_ENGINE_PRINCIPAL, locked_for("layout_engine")
    )
    version, api = build.layout_engine_versions(le_fetched.directory)

    fetched: Dict[str, Fetched] = {}
    manifests: List[extension_manifest.Manifest] = []
    for name, source in project.extensions.items():
        signers = _publisher_signers(state, project, source.publisher) if source.publisher else None
        fetched[name] = _fetch(state, name, source, signers, source.publisher, locked_for(name))
        try:
            manifest = extension_manifest.load(fetched[name].directory)
        except extension_manifest.ManifestError as e:
            raise InstallError(str(e)) from e
        if manifest.name != name:
            raise InstallError(f"le_project.toml calls it {name}, but its manifest names it {manifest.name}")
        manifests.append(manifest)
    for manifest in manifests:
        for dependency in manifest.dependencies:
            if dependency not in project.extensions:
                raise InstallError(f"{manifest.name} depends on {dependency}: add it to le_project.toml (`le add {dependency} ...`)")
    try:
        ordered = extension_manifest.check_and_order(manifests, version, api)
    except extension_manifest.ManifestError as e:
        raise InstallError(str(e)) from e

    new = lockfile.Lock(
        project_hash=current_hash,
        layout_engine_version=version,
        extension_api=api,
        layout_engine=le_fetched.locked,
        extensions=[
            lockfile.LockedExtension(
                name=m.name,
                version=m.version,
                tier="compiled" if m.compiled else "script",
                locked=fetched[m.name].locked,
                dependencies=sorted(m.dependencies),
            )
            for m in ordered
        ],
    )
    lockfile.save(project.root, new)

    bundle = build.build(
        le_fetched.directory,
        [fetched[m.name].directory for m in ordered],
        state,
        project.build_type,
        project.jobs,
        version,
        project.startup,
    )
    _write_stamp(state, project, new)
    return bundle


# --- Staleness, for `le shell` -------------------------------------------------


def _path_sources_mtime(project: projectfile.Project) -> float:
    """The newest file under any path extension (skipping hidden directories), or
    the startup script. A path Layout Engine checkout isn't scanned - it's
    large, and rebuilt with an explicit `le install`."""
    newest = 0.0
    for directory in (s.path for s in project.extensions.values() if s.path is not None):
        for p in directory.rglob("*"):
            if any(part.startswith(".") for part in p.relative_to(directory).parts):
                continue
            if p.is_file():
                newest = max(newest, p.stat().st_mtime)
    if project.startup is not None and project.startup.exists():
        newest = max(newest, project.startup.stat().st_mtime)
    return newest


def _stamp(project: projectfile.Project, lock: lockfile.Lock) -> dict:
    return {"project_hash": lock.project_hash, "lock": lockfile.dumps(lock), "path_sources_mtime": _path_sources_mtime(project)}


def _write_stamp(state: Path, project: projectfile.Project, lock: lockfile.Lock) -> None:
    (state / "installed.json").write_text(json.dumps(_stamp(project, lock)))


def is_current(root: Path) -> bool:
    """Whether .le/bundle matches le_project.toml, the lock and every path source."""
    project = projectfile.load(root)
    state = project.root / STATE_DIR
    lock = lockfile.load(project.root)
    stamp_file = state / "installed.json"
    if lock is None or not (state / "bundle" / "le_shell").exists() or not stamp_file.is_file():
        return False
    if lock.project_hash != lockfile.project_hash(project.file):
        return False
    return json.loads(stamp_file.read_text()) == _stamp(project, lock)
