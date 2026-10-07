"""The `le` command line: init, add, remove, trust, install, update, list, shell, bundle, and the authoring
commands new-extension, test, makemigration and check."""

import argparse
import os
import sys
from pathlib import Path
from typing import List, Optional

from le import authoring, build, install as installer, lock as lockfile, project as projectfile, releases, sources


class CliError(Exception):
    pass


def find_root(start: Path) -> Path:
    """The nearest directory, from `start` up, holding le_project.toml."""
    for directory in [start, *start.parents]:
        if (directory / projectfile.PROJECT_FILE).is_file():
            return directory
    raise CliError(f"no {projectfile.PROJECT_FILE} here or above - run `le init` first")


def _edit(root: Path, edit) -> None:
    path = root / projectfile.PROJECT_FILE
    text = edit(path.read_text())
    previous = path.read_text()
    path.write_text(text)
    try:
        projectfile.load(root)  # refuse an edit that leaves the file invalid
    except projectfile.ProjectError:
        path.write_text(previous)
        raise


def cmd_init(args) -> int:
    root = Path(args.directory).resolve()
    root.mkdir(parents=True, exist_ok=True)
    path = root / projectfile.PROJECT_FILE
    if path.exists():
        raise CliError(f"{path} already exists")
    if args.layout_engine_path:
        layout_engine = {"path": os.path.relpath(Path(args.layout_engine_path).resolve(), root)}
    else:
        major, minor, _ = installer.own_layout_engine_version().split(".")
        layout_engine = {"github": "drummondj/layout_engine", "version": f">={major}.{minor}, <{major}.{int(minor) + 1}"}
    path.write_text(projectfile.template(args.name or root.name, layout_engine))
    gitignore = root / ".gitignore"
    existing = gitignore.read_text() if gitignore.exists() else ""
    if ".le/" not in existing.splitlines():
        gitignore.write_text(existing + ("" if existing.endswith("\n") or not existing else "\n") + ".le/\n")
    print(f"le: created {path}")
    return 0


def cmd_add(args) -> int:
    root = find_root(Path.cwd())
    if args.path:
        # Checked now, so a wrong path fails here rather than at the next build.
        manifest = installer.extension_manifest.load(Path(args.path).resolve())
        if manifest.name != args.name:
            raise CliError(f"{args.path}: its manifest names it {manifest.name}, not {args.name}")
        entry = {"path": os.path.relpath(Path(args.path).resolve(), root)}
    else:
        if not (args.tag or args.rev or args.version):
            raise CliError("a --github source needs --tag, --rev or --version")
        pin = {"tag": args.tag} if args.tag else {"rev": args.rev} if args.rev else {"version": args.version}
        entry = {"github": args.github, **pin}
        if args.publisher:
            entry["publisher"] = args.publisher
        if args.allow_unsigned:
            entry["allow_unsigned"] = True
    _edit(root, lambda text: projectfile.set_entry(text, "extensions", args.name, projectfile.inline_table(entry)))
    print(f"le: added {args.name} - `le shell`, `le test` or `le install` builds it")
    return 0


def cmd_remove(args) -> int:
    root = find_root(Path.cwd())
    users = [name for name in projectfile.load(root).extensions if name != args.name and args.name in _dependencies(root, name)]
    if users:
        raise CliError(f"{', '.join(users)} depend{'s' if len(users) == 1 else ''} on {args.name} - remove {'it' if len(users) == 1 else 'them'} first")
    _edit(root, lambda text: projectfile.remove_entry(text, "extensions", args.name))
    print(f"le: removed {args.name}")
    return 0


def _dependencies(root: Path, name: str) -> List[str]:
    """What extension `name` depends on: a path extension's manifest says now; otherwise the lock, if it was installed."""
    source = projectfile.load(root).extensions[name]
    if source.path is not None:
        try:
            return list(installer.extension_manifest.load(source.path).dependencies)
        except installer.extension_manifest.ManifestError:
            return []
    lock = lockfile.load(root)
    entry = lock.extension(name) if lock is not None else None
    return entry.dependencies if entry is not None else []


def cmd_trust(args) -> int:
    root = find_root(Path.cwd())
    key = Path(args.key[1:]).read_text().strip() if args.key.startswith("@") else args.key.strip()
    if not key.startswith("ssh-"):
        raise CliError("expected an SSH public key (ssh-ed25519 AAAA...) or @file")
    keys = projectfile.load(root).trust.get(args.publisher, [])
    if key not in keys:
        keys = keys + [key]
    value = "[" + ", ".join(projectfile._quote(k) for k in keys) + "]"
    _edit(root, lambda text: projectfile.set_entry(text, "trust", args.publisher, value))
    print(f"le: {args.publisher} now has {len(keys)} trusted key{'s' if len(keys) != 1 else ''}")
    return 0


def _install(root: Path, update, allow_downgrade: bool) -> int:
    le_shell = installer.install(root, update, allow_downgrade)
    print(f"le: ready - `le shell` runs {le_shell}")
    return 0


def cmd_install(args) -> int:
    return _install(find_root(Path.cwd()), None, args.allow_downgrade)


def cmd_update(args) -> int:
    return _install(find_root(Path.cwd()), set(args.names), args.allow_downgrade)


def cmd_list(args) -> int:
    root = find_root(Path.cwd())
    lock = lockfile.load(root)
    if lock is None:
        raise CliError("nothing installed yet - run `le install`")
    how = "release" if lock.layout_engine_bundle == "release" else "built from source"
    print(f"layout_engine {lock.layout_engine_version}  ({how})  {_describe(lock.layout_engine)}")
    for e in lock.extensions:
        print(f"{e.name} {e.version}  {e.tier}  {_describe(e.locked)}")
    return 0


def _describe(locked: lockfile.LockedSource) -> str:
    text = locked.source
    if locked.tag:
        text += f"@{locked.tag}"
    elif locked.rev:
        text += f"@{locked.rev[:12]}"
    if locked.unsigned:
        text += "  UNSIGNED"
    elif locked.signer:
        text += f"  signed {locked.signer}"
    return text


def cmd_shell(args) -> int:
    root = find_root(Path.cwd())
    command = _installed_shell(root)
    os.execv(command[0], [*command, *args.args])
    return 0  # not reached


def _installed_shell(root: Path) -> List[str]:
    command = installer.shell_command(root)
    if command is None:
        _install(root, None, False)
        command = installer.shell_command(root)
    return command


def cmd_bundle(args) -> int:
    root = find_root(Path.cwd())
    _installed_shell(root)
    destination = installer.export_bundle(root, Path(args.directory).resolve())
    print(f"le: bundled into {destination} - run {destination / 'le_shell'}")
    return 0


def cmd_new_extension(args) -> int:
    directory = authoring.new_extension(args.name, Path(args.directory or args.name).resolve(), args.script, args.prefix, args.description)
    print(f"le: created {directory}")
    try:
        root = find_root(Path.cwd())
    except CliError:
        root = None
    if root is not None:
        print(f"le: next: `le add {args.name} --path {os.path.relpath(directory, root)}` (in {root}), then `le test {args.name}`")
    else:
        print("le: next, make this directory a project (see docs/EXTENSION_SDK.md) and add it:")
        print(f"le:   `le init --layout-engine-path <layout_engine checkout>`, then `le add {args.name} --path {os.path.relpath(directory)}`")
    return 0


def cmd_test(args) -> int:
    root = find_root(Path.cwd())
    return authoring.run_tests(root, args.names, _installed_shell(root))


def cmd_makemigration(args) -> int:
    root = find_root(Path.cwd())
    return authoring.make_migration(root, args.extension, args.name, args.non_interactive)


def cmd_check(args) -> int:
    root = find_root(Path.cwd())
    return authoring.check(_installed_shell(root), Path(args.file))


def parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(prog="le", description="The Layout Engine package manager.")
    sub = p.add_subparsers(dest="command", required=True)

    s = sub.add_parser("init", help="create le_project.toml in a directory")
    s.add_argument("directory", nargs="?", default=".")
    s.add_argument("--name", help="project name (default: the directory's)")
    s.add_argument("--layout-engine-path", help="build a local Layout Engine checkout instead of a release tag")
    s.set_defaults(func=cmd_init)

    s = sub.add_parser("add", help="add an extension to le_project.toml (the next shell/test/install builds it)")
    s.add_argument("name")
    where = s.add_mutually_exclusive_group(required=True)
    where.add_argument("--github", metavar="OWNER/REPO")
    where.add_argument("--path", metavar="DIR")
    pin = s.add_mutually_exclusive_group()
    pin.add_argument("--tag")
    pin.add_argument("--rev")
    pin.add_argument("--version", help='a range matched against vX.Y.Z tags, e.g. ">=1.4, <2"')
    s.add_argument("--publisher", help="whose key in [trust] signs it (github sources)")
    s.add_argument("--allow-unsigned", action="store_true", help="accept it unsigned - for development only")
    s.set_defaults(func=cmd_add)

    s = sub.add_parser("remove", help="remove an extension from le_project.toml")
    s.add_argument("name")
    s.set_defaults(func=cmd_remove)

    s = sub.add_parser("trust", help="trust an SSH key to sign a publisher's releases")
    s.add_argument("publisher")
    s.add_argument("key", help="the public key, or @file")
    s.set_defaults(func=cmd_trust)

    s = sub.add_parser("install", help="install what the lock (or le_project.toml) says")
    s.add_argument("--allow-downgrade", action="store_true", help="accept a lower version than the lock has")
    s.set_defaults(func=cmd_install)

    s = sub.add_parser("update", help="re-resolve entries ignoring the lock (all if none named), then install")
    s.add_argument("names", nargs="*")
    s.add_argument("--allow-downgrade", action="store_true", help="accept a lower version than the lock has")
    s.set_defaults(func=cmd_update)

    s = sub.add_parser("list", help="what's installed")
    s.set_defaults(func=cmd_list)

    s = sub.add_parser("shell", help="run the project's le_shell (installing first if needed)")
    s.add_argument("args", nargs=argparse.REMAINDER)
    s.set_defaults(func=cmd_shell)

    s = sub.add_parser("bundle", help="copy the installed project into a self-contained directory, e.g. for deployment")
    s.add_argument("directory", help="where to put it (new, or empty)")
    s.set_defaults(func=cmd_bundle)

    s = sub.add_parser("new-extension", help="create an extension directory from the examples")
    s.add_argument("name", help="snake_case, e.g. acme_router")
    s.add_argument("--script", action="store_true", help="Tcl procs only (no compiler needed), from hello_script; default: compiled, from hello_ext")
    s.add_argument("--prefix", help="PascalCase prefix for its classes and purposes (default: from the name, e.g. AcmeRouter)")
    s.add_argument("--description", help="the manifest's description")
    s.add_argument("--directory", help="where to create it (default: ./<name>)")
    s.set_defaults(func=cmd_new_extension)

    s = sub.add_parser("test", help="build and run extensions' tests (all if none named)")
    s.add_argument("names", nargs="*")
    s.set_defaults(func=cmd_test)

    s = sub.add_parser("makemigration", help="draft a path extension's next schema migration")
    s.add_argument("extension")
    s.add_argument("--name", required=True, help="what changed, e.g. note_text_to_body")
    s.add_argument("--non-interactive", action="store_true", help="leave possible renames as TODOs instead of asking")
    s.set_defaults(func=cmd_makemigration)

    s = sub.add_parser("check", help="whether a .led file's extensions load in this project")
    s.add_argument("file")
    s.set_defaults(func=cmd_check)
    return p


def main(argv: Optional[List[str]] = None) -> int:
    args = parser().parse_args(argv)
    try:
        return args.func(args)
    except (
        CliError,
        projectfile.ProjectError,
        lockfile.LockError,
        sources.SourceError,
        installer.InstallError,
        build.BuildError,
        releases.ReleaseError,
        authoring.AuthoringError,
        authoring.extension_manifest.ManifestError,
    ) as e:
        print(f"le: error: {e}", file=sys.stderr)
        return 1
