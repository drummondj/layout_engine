"""
`le install`: resolve the project's sources (or replay the lock), check
signatures and manifests, write the lock, and make the project runnable.

Resolution happens when there's no lock, le_project.toml changed since it
was written, or `le update` asks for it. Otherwise the lock is replayed: each
GitHub source must still resolve to its locked commit, signed by its locked
key, so a moved tag or a swapped key stops the install. Re-resolving never
silently lowers a version the lock already has (the downgrade rule): files
written by the newer version might not load in the older one.

A project with only script extensions and a released Layout Engine (a tag
or version range on GitHub) uses the signed release bundle: nothing is
compiled. Anything else - a compiled extension, a local checkout, a
revision pin - builds Layout Engine from source.
"""

import json
import os
import shutil
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Set

from le import build, lock as lockfile, project as projectfile, releases, sources

_FROZEN = getattr(sys, "frozen", False)  # running as the PyInstaller binary
_REPO_ROOT = Path(__file__).resolve().parents[3]
if not _FROZEN:
    sys.path.insert(0, str(_REPO_ROOT / "codegen"))
from codegen import extension_manifest  # noqa: E402

LAYOUT_ENGINE_PRINCIPAL = "release@layout-engine"
STATE_DIR = ".le"


class InstallError(Exception):
    pass


def _data_dir() -> Path:
    """Where the PyInstaller binary keeps its bundled data files."""
    return Path(getattr(sys, "_MEIPASS", "."))


def layout_engine_allowed_signers() -> Path:
    """The release key `le` trusts for Layout Engine's own tags and release bundles
    (LE_RELEASE_ALLOWED_SIGNERS overrides it, for tests)."""
    if os.environ.get("LE_RELEASE_ALLOWED_SIGNERS"):
        return Path(os.environ["LE_RELEASE_ALLOWED_SIGNERS"])
    path = _data_dir() / "keys" / "allowed_signers" if _FROZEN else _REPO_ROOT / "assets" / "keys" / "allowed_signers"
    if not path.is_file():
        raise InstallError(f"Layout Engine's release key isn't at {path}")
    return path


def own_layout_engine_version() -> str:
    """The Layout Engine version this `le` came with."""
    if _FROZEN:
        return (_data_dir() / "le_version.txt").read_text().strip()
    return build.layout_engine_versions(_REPO_ROOT)[0]


@dataclass
class Fetched:
    directory: Optional[Path]  # None for a release Layout Engine, whose source isn't needed
    locked: lockfile.LockedSource


def _resolve_github(name: str, source: projectfile.Source, allowed_signers: Optional[Path], principal: Optional[str],
                    locked: Optional[lockfile.LockedSource]):
    """(mirror, commit, tag or None, signer) for a GitHub source, checked against the lock if there is one."""
    repo_dir = sources.mirror(source.github)
    tag = source.tag
    if source.version is not None:
        tag = locked.tag if locked is not None and locked.tag else sources.best_tag(repo_dir, source.version, extension_manifest.satisfies)
    commit, ref, is_tag = sources.resolve(repo_dir, tag, source.rev)
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
    return repo_dir, commit, tag, signer


def _fetch_extension(state: Path, name: str, source: projectfile.Source, allowed_signers: Optional[Path],
                     principal: Optional[str], locked: Optional[lockfile.LockedSource]) -> Fetched:
    """Makes an extension available under .le/src/<name> (or in place, for a path)."""
    if source.path is not None:
        if not source.path.is_dir():
            raise InstallError(f"{name}: {source.path} isn't a directory")
        return Fetched(source.path, lockfile.LockedSource(source=f"path:{source.path}"))
    repo_dir, commit, tag, signer = _resolve_github(name, source, allowed_signers, principal, locked)
    directory = state / "src" / name
    sources.export(repo_dir, commit, directory)
    return Fetched(directory, lockfile.LockedSource(source=f"github:{source.github}", rev=commit, tag=tag, signer=signer,
                                                    unsigned=source.allow_unsigned))


def _publisher_signers(state: Path, project: projectfile.Project, publisher: str) -> Path:
    return sources.write_allowed_signers(state / "trust" / f"{publisher}.allowed_signers", publisher, project.trust[publisher])


def _check_downgrades(old: Optional[lockfile.Lock], le_version: str, manifests: List[extension_manifest.Manifest],
                      allow_downgrade: bool) -> None:
    if old is None or allow_downgrade:
        return
    lowered = []
    if extension_manifest.parse_version(le_version) < extension_manifest.parse_version(old.layout_engine_version):
        lowered.append(f"layout_engine {old.layout_engine_version} -> {le_version}")
    for m in manifests:
        before = old.extension(m.name)
        if before is not None and extension_manifest.parse_version(m.version) < extension_manifest.parse_version(before.version):
            lowered.append(f"{m.name} {before.version} -> {m.version}")
    if lowered:
        raise InstallError(
            "this would downgrade " + ", ".join(lowered) + ". Files saved since might not open in the older version; "
            "pass --allow-downgrade if you're sure."
        )


def install(root: Path, update: Optional[Set[str]] = None, allow_downgrade: bool = False) -> Path:
    """
    Installs the project at `root`; returns the le_shell to run. `update`
    names entries to re-resolve ignoring the lock ("layout_engine" or
    extension names); an empty set means all of them.
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

    # Extensions first: whether any is compiled decides how Layout Engine comes.
    fetched: Dict[str, Fetched] = {}
    manifests: List[extension_manifest.Manifest] = []
    for name, source in project.extensions.items():
        signers = _publisher_signers(state, project, source.publisher) if source.publisher else None
        fetched[name] = _fetch_extension(state, name, source, signers, source.publisher, locked_for(name))
        try:
            manifest = extension_manifest.load(fetched[name].directory)
        except extension_manifest.ManifestError as e:
            raise InstallError(str(e)) from e
        if manifest.name != name:
            raise InstallError(f"le_project.toml calls it {name}, but its manifest names it {manifest.name}")
        tag = fetched[name].locked.tag
        if tag is not None and tag.startswith("v") and tag[1:] != manifest.version:
            raise InstallError(f"{name}: tag {tag} holds a manifest saying version {manifest.version}")
        manifests.append(manifest)
    for manifest in manifests:
        for dependency in manifest.dependencies:
            if dependency not in project.extensions:
                raise InstallError(f"{manifest.name} depends on {dependency}: add it to le_project.toml (`le add {dependency} ...`)")

    le_source = project.layout_engine
    use_release = (
        not any(m.compiled for m in manifests) and le_source.github is not None and le_source.rev is None and not le_source.allow_unsigned
    )
    le_locked_old = locked_for("layout_engine")
    if le_locked_old is not None and old.layout_engine_bundle != ("release" if use_release else "source"):
        le_locked_old = None  # switching between a release and a source build re-resolves
    release_sha256 = None
    if use_release:
        repo_dir, commit, tag, signer = _resolve_github("layout_engine", le_source, layout_engine_allowed_signers(), LAYOUT_ENGINE_PRINCIPAL,
                                                        le_locked_old)
        bundle_dir, release_sha256 = releases.fetch(le_source.github, tag, layout_engine_allowed_signers(), LAYOUT_ENGINE_PRINCIPAL,
                                                    old.release_sha256 if le_locked_old is not None else None)
        version, api = releases.bundle_versions(bundle_dir)
        le_fetched = Fetched(None, lockfile.LockedSource(source=f"github:{le_source.github}", rev=commit, tag=tag, signer=signer))
    elif le_source.path is not None:
        if not le_source.path.is_dir():
            raise InstallError(f"layout_engine: {le_source.path} isn't a directory")
        le_fetched = Fetched(le_source.path, lockfile.LockedSource(source=f"path:{le_source.path}"))
        version, api = build.layout_engine_versions(le_source.path)
    else:
        repo_dir, commit, tag, signer = _resolve_github("layout_engine", le_source, layout_engine_allowed_signers(), LAYOUT_ENGINE_PRINCIPAL,
                                                        le_locked_old)
        sources.export(repo_dir, commit, state / "src" / "layout_engine")
        le_fetched = Fetched(state / "src" / "layout_engine",
                             lockfile.LockedSource(source=f"github:{le_source.github}", rev=commit, tag=tag, signer=signer,
                                                   unsigned=le_source.allow_unsigned))
        version, api = build.layout_engine_versions(le_fetched.directory)

    try:
        ordered = extension_manifest.check_and_order(manifests, version, api)
    except extension_manifest.ManifestError as e:
        raise InstallError(str(e)) from e
    _check_downgrades(old, version, ordered, allow_downgrade)

    new = lockfile.Lock(
        project_hash=current_hash,
        layout_engine_version=version,
        extension_api=api,
        layout_engine=le_fetched.locked,
        layout_engine_bundle="release" if use_release else "source",
        release_sha256=release_sha256,
        extensions=[
            lockfile.LockedExtension(name=m.name, version=m.version, tier="compiled" if m.compiled else "script",
                                     locked=fetched[m.name].locked, dependencies=sorted(m.dependencies))
            for m in ordered
        ],
    )
    lockfile.save(project.root, new)

    if use_release:
        print(f"le: using the Layout Engine {version} release - nothing to compile", flush=True)
        le_shell = bundle_dir / "le_shell"
        index = _write_script_index(state, ordered, version, api, project.startup)
        command = [str(le_shell), "-extensions", str(index)]
    else:
        bundle = build.build(le_fetched.directory, [fetched[m.name].directory for m in ordered], state, project.build_type,
                             project.jobs, version, project.startup)
        command = [str(bundle / "le_shell")]
    _write_stamp(state, project, new, command)
    return Path(command[0])


def _write_script_index(state: Path, ordered: List[extension_manifest.Manifest], version: str, api: int,
                        startup: Optional[Path]) -> Path:
    """The project's own extensions.json for a release le_shell: each script
    extension's procs and resources copied under .le/ext/<name>/."""
    ext_root = state / "ext"
    if ext_root.exists():
        shutil.rmtree(ext_root)
    entries = []
    for m in ordered:
        target = ext_root / m.name
        procs = []
        for path in [*m.tcl_procs, *m.resources]:
            relative = path.relative_to(m.directory)
            destination = target / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            if path.is_dir():
                shutil.copytree(path, destination)
            else:
                shutil.copy2(path, destination)
            if path in m.tcl_procs:
                procs.append(str(relative))
        entries.append({"name": m.name, "version": m.version, "tier": "script", "dir": f"ext/{m.name}", "procs": procs})
    index = {"format": 1, "layout_engine": version, "extension_api": api, "extensions": entries}
    if startup is not None:
        index["startup"] = str(Path("..") / startup.relative_to(state.parent)) if startup.is_relative_to(state.parent) else str(startup)
    path = state / "extensions.json"
    path.write_text(json.dumps(index, indent=2) + "\n")
    return path


# --- Staleness and the command to run, for `le shell` -----------------------------


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


def _write_stamp(state: Path, project: projectfile.Project, lock: lockfile.Lock, command: List[str]) -> None:
    (state / "installed.json").write_text(json.dumps({**_stamp(project, lock), "command": command}))


def shell_command(root: Path) -> Optional[List[str]]:
    """The le_shell command line for the installed project, if it's current
    (matches le_project.toml, the lock and every path source); else None."""
    project = projectfile.load(root)
    state = project.root / STATE_DIR
    lock = lockfile.load(project.root)
    stamp_file = state / "installed.json"
    if lock is None or not stamp_file.is_file() or lock.project_hash != lockfile.project_hash(project.file):
        return None
    stamp = json.loads(stamp_file.read_text())
    command = stamp.pop("command", None)
    if not command or not Path(command[0]).exists() or stamp != _stamp(project, lock):
        return None
    return command


def is_current(root: Path) -> bool:
    return shell_command(root) is not None
