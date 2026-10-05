"""The `le` command line: init, add, remove, trust, install, update, list, shell."""

import argparse
import os
import sys
from pathlib import Path
from typing import List, Optional

from le import build, install as installer, lock as lockfile, project as projectfile, sources


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
        version, _ = build.layout_engine_versions(installer._REPO_ROOT)
        layout_engine = {"github": "drummondj/layout_engine", "tag": f"v{version}"}
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
        if not (args.tag or args.rev):
            raise CliError("a --github source needs --tag or --rev")
        entry = {"github": args.github, **({"tag": args.tag} if args.tag else {"rev": args.rev})}
        if args.publisher:
            entry["publisher"] = args.publisher
        if args.allow_unsigned:
            entry["allow_unsigned"] = True
    _edit(root, lambda text: projectfile.set_entry(text, "extensions", args.name, projectfile.inline_table(entry)))
    print(f"le: added {args.name}")
    return 0 if args.no_install else _install(root, None)


def cmd_remove(args) -> int:
    root = find_root(Path.cwd())
    lock = lockfile.load(root)
    if lock is not None:
        users = [e.name for e in lock.extensions if args.name in e.dependencies]
        if users:
            raise CliError(f"{', '.join(users)} depend{'s' if len(users) == 1 else ''} on {args.name} - remove {'it' if len(users) == 1 else 'them'} first")
    _edit(root, lambda text: projectfile.remove_entry(text, "extensions", args.name))
    print(f"le: removed {args.name}")
    return 0 if args.no_install else _install(root, None)


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


def _install(root: Path, update) -> int:
    bundle = installer.install(root, update)
    print(f"le: installed into {bundle}")
    return 0


def cmd_install(args) -> int:
    return _install(find_root(Path.cwd()), None)


def cmd_update(args) -> int:
    return _install(find_root(Path.cwd()), set(args.names))


def cmd_list(args) -> int:
    root = find_root(Path.cwd())
    lock = lockfile.load(root)
    if lock is None:
        raise CliError("nothing installed yet - run `le install`")
    print(f"layout_engine {lock.layout_engine_version}  {_describe(lock.layout_engine)}")
    for e in lock.extensions:
        print(f"{e.name} {e.version}  {e.tier}  {_describe(e.locked)}")
    return 0


def _describe(locked: lockfile.LockedSource) -> str:
    text = locked.source
    if locked.rev:
        text += f"@{locked.rev[:12]}"
    if locked.unsigned:
        text += "  UNSIGNED"
    elif locked.signer:
        text += f"  signed {locked.signer}"
    return text


def cmd_shell(args) -> int:
    root = find_root(Path.cwd())
    if not installer.is_current(root):
        _install(root, None)
    le_shell = root / installer.STATE_DIR / "bundle" / "le_shell"
    os.execv(str(le_shell), [str(le_shell), *args.args])
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

    s = sub.add_parser("install", help="build and install what the lock (or le_project.toml) says")
    s.set_defaults(func=cmd_install)

    s = sub.add_parser("update", help="re-resolve entries ignoring the lock (all if none named), then install")
    s.add_argument("names", nargs="*")
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
    ) as e:
        print(f"le: error: {e}", file=sys.stderr)
        return 1
