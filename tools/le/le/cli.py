"""The `le` command line: init, add, remove, trust, install, update, list, shell."""

import argparse
import os
import sys
from pathlib import Path
from typing import List, Optional

from le import build, install as installer, lock as lockfile, project as projectfile, releases, sources


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
    print(f"le: added {args.name}")
    return 0 if args.no_install else _install(root, None, False)


def cmd_remove(args) -> int:
    root = find_root(Path.cwd())
    lock = lockfile.load(root)
    if lock is not None:
        users = [e.name for e in lock.extensions if args.name in e.dependencies]
        if users:
            raise CliError(f"{', '.join(users)} depend{'s' if len(users) == 1 else ''} on {args.name} - remove {'it' if len(users) == 1 else 'them'} first")
    _edit(root, lambda text: projectfile.remove_entry(text, "extensions", args.name))
    print(f"le: removed {args.name}")
    return 0 if args.no_install else _install(root, None, False)


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
    command = installer.shell_command(root)
    if command is None:
        _install(root, None, False)
        command = installer.shell_command(root)
    os.execv(command[0], [*command, *args.args])
    return 0  # not reached


def parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(prog="le", description="The Layout Engine package manager.")
    sub = p.add_subparsers(dest="command", required=True)

    s = sub.add_parser("init", help="create le_project.toml in a directory")
    s.add_argument("directory", nargs="?", default=".")
    s.add_argument("--name", help="project name (default: the directory's)")
    s.add_argument("--layout-engine-path", help="build a local Layout Engine checkout instead of a release tag")
    s.set_defaults(func=cmd_init)

    s = sub.add_parser("add", help="add an extension, then install")
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
    s.add_argument("--no-install", action="store_true")
    s.set_defaults(func=cmd_add)

    s = sub.add_parser("remove", help="remove an extension, then install")
    s.add_argument("name")
    s.add_argument("--no-install", action="store_true")
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
    ) as e:
        print(f"le: error: {e}", file=sys.stderr)
        return 1
