"""
Schema migrations (docs/NATIVE_FILE_FORMAT_RESEARCH.md §4).

A migration file describes how data shaped like schema version N-1 becomes
data shaped like version N, as an ordered list of typed ops:

    # src/database/migrations/0001_layer_kind.py
    from codegen.migration import *

    migration = Migration(
        from_version="0.49.0",
        to_version="0.50.0",
        description="Layer.type renamed to Layer.kind",
        ops=[RenameField("Layer", "type", "kind")],
    )

Every op has a *symbolic* effect on a schema descriptor (descriptor.py) -
checkmigrations replays the whole chain from the oldest snapshot and
requires each step to land exactly on that version's snapshot, and the
last on the current schema. Some ops also have a *runtime* effect the
native file loader applies to an older file's embedded schema before it
matches fields by name (renames); ops the loader can't apply to data yet
are flagged, so an old file needing one is refused with a clear message
rather than misread.

`codegen --target makemigration` drafts the next migration by diffing
the newest snapshot against the current schema.
"""

import copy
import pprint
import re
from dataclasses import dataclass, field
import importlib.util
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

from codegen import descriptor as d


class MigrationError(Exception):
    pass


# --- Helpers over a descriptor dict -------------------------------------------


def _classes(desc: Dict[str, Any]) -> List[Dict[str, Any]]:
    return desc["classes"]


def _find_class(desc: Dict[str, Any], name: str) -> Dict[str, Any]:
    for klass in _classes(desc):
        if klass["name"] == name:
            return klass
    raise MigrationError(f"no class {name}")


def _has_class(desc: Dict[str, Any], name: str) -> bool:
    return any(klass["name"] == name for klass in _classes(desc))


def _members(klass: Dict[str, Any]) -> List[Dict[str, Any]]:
    """A class's fields, or an enum's values."""
    if klass["kind"] == "enum":
        return klass["values"]
    return klass["fields"]


def _find_member(klass: Dict[str, Any], name: str) -> Dict[str, Any]:
    for member in _members(klass):
        if member["name"] == name:
            return member
    raise MigrationError(f"{klass['name']} has no {'value' if klass['kind'] == 'enum' else 'field'} {name}")


def _has_member(klass: Dict[str, Any], name: str) -> bool:
    return any(member["name"] == name for member in _members(klass))


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise MigrationError(message)


# --- Ops -------------------------------------------------------------------------

# How the native file loader treats an op (see migrations_hpp_j2.py):
RUNTIME_NONE = "none"  # nothing to do: name matching covers it (adds, removes, compatible retypes)
RUNTIME_RENAME_CLASS = "rename_class"
RUNTIME_RENAME_FIELD = "rename_field"
RUNTIME_RENAME_ENUM_VALUE = "rename_enum_value"
RUNTIME_UNSUPPORTED = "unsupported"  # needs the generic data runtime (not built yet)


class Op:
    runtime = RUNTIME_NONE

    def apply(self, desc: Dict[str, Any]) -> None:
        raise NotImplementedError

    def runtime_entries(self) -> List[Tuple[str, str, str, str]]:
        """(kind, class, old name, new name) entries for the loader's table."""
        return []

    def render(self) -> str:
        args = ", ".join(pprint.pformat(v, width=100, sort_dicts=False) for v in self._render_args())
        return f"{type(self).__name__}({args})"

    def _render_args(self) -> List[Any]:
        return list(vars(self).values())

    def __repr__(self) -> str:
        return self.render()


class AddClass(Op):
    """A new class (pooled, struct or enum), given as its descriptor entry."""

    def __init__(self, klass: Dict[str, Any]):
        self.klass = klass

    def apply(self, desc):
        _require(not _has_class(desc, self.klass["name"]), f"AddClass: {self.klass['name']} already exists")
        _classes(desc).append(copy.deepcopy(self.klass))


class RemoveClass(Op):
    def __init__(self, name: str):
        self.name = name

    def apply(self, desc):
        klass = _find_class(desc, self.name)
        _classes(desc).remove(klass)


class RenameClass(Op):
    """Renames a class; every field referring to it follows."""

    runtime = RUNTIME_RENAME_CLASS

    def __init__(self, old: str, new: str):
        self.old = old
        self.new = new

    def apply(self, desc):
        klass = _find_class(desc, self.old)
        _require(not _has_class(desc, self.new), f"RenameClass: {self.new} already exists")
        klass["name"] = self.new
        for other in _classes(desc):
            if other["kind"] != "enum":
                for f in other["fields"]:
                    if f["type"] == self.old:
                        f["type"] = self.new

    def runtime_entries(self):
        return [(self.runtime, "", self.old, self.new)]


class AddField(Op):
    """A new field, given as its descriptor entry (see descriptor.py)."""

    def __init__(self, klass: str, field: Dict[str, Any]):
        self.klass = klass
        self.field = field

    def apply(self, desc):
        klass = _find_class(desc, self.klass)
        _require(klass["kind"] != "enum", f"AddField: {self.klass} is an enum (use AddEnumValue)")
        _require(not _has_member(klass, self.field["name"]), f"AddField: {self.klass}.{self.field['name']} already exists")
        klass["fields"].append(copy.deepcopy(self.field))


class RemoveField(Op):
    def __init__(self, klass: str, name: str):
        self.klass = klass
        self.name = name

    def apply(self, desc):
        klass = _find_class(desc, self.klass)
        klass["fields"].remove(_find_member(klass, self.name))


class RenameField(Op):
    runtime = RUNTIME_RENAME_FIELD

    def __init__(self, klass: str, old: str, new: str):
        self.klass = klass
        self.old = old
        self.new = new

    def apply(self, desc):
        klass = _find_class(desc, self.klass)
        _require(not _has_member(klass, self.new), f"RenameField: {self.klass}.{self.new} already exists")
        _find_member(klass, self.old)["name"] = self.new

    def runtime_entries(self):
        return [(self.runtime, self.klass, self.old, self.new)]


class AlterField(Op):
    """
    A field's type or flags change, given as its new descriptor entry. The
    loader reads old data into the new shape when the storage allows it
    (int -> float, T -> optional<T> -> list<T>); anything else is refused
    as needing a conversion (a future ConvertField).
    """

    def __init__(self, klass: str, field: Dict[str, Any]):
        self.klass = klass
        self.field = field

    def apply(self, desc):
        klass = _find_class(desc, self.klass)
        fields = klass["fields"]
        fields[fields.index(_find_member(klass, self.field["name"]))] = copy.deepcopy(self.field)


class AddEnumValue(Op):
    def __init__(self, enum: str, name: str, value: int):
        self.enum = enum
        self.name = name
        self.value = value

    def apply(self, desc):
        klass = _find_class(desc, self.enum)
        _require(klass["kind"] == "enum", f"AddEnumValue: {self.enum} is not an enum")
        _require(not _has_member(klass, self.name), f"AddEnumValue: {self.enum}.{self.name} already exists")
        klass["values"].append({"name": self.name, "value": self.value})


class RemoveEnumValue(Op):
    """
    Removes an enum value. With `map_to`, stored data holding it reads as
    that value instead; without, a file holding it is refused.
    """

    def __init__(self, enum: str, name: str, map_to: Optional[str] = None):
        self.enum = enum
        self.name = name
        self.map_to = map_to

    def apply(self, desc):
        klass = _find_class(desc, self.enum)
        klass["values"].remove(_find_member(klass, self.name))
        if self.map_to is not None:
            _find_member(klass, self.map_to)

    def runtime_entries(self):
        if self.map_to is None:
            return []
        return [(RUNTIME_RENAME_ENUM_VALUE, self.enum, self.name, self.map_to)]


class RenameEnumValue(Op):
    runtime = RUNTIME_RENAME_ENUM_VALUE

    def __init__(self, enum: str, old: str, new: str):
        self.enum = enum
        self.old = old
        self.new = new

    def apply(self, desc):
        klass = _find_class(desc, self.enum)
        _require(not _has_member(klass, self.new), f"RenameEnumValue: {self.enum}.{self.new} already exists")
        _find_member(klass, self.old)["name"] = self.new

    def runtime_entries(self):
        return [(self.runtime, self.enum, self.old, self.new)]


class AlterEnumValue(Op):
    """Renumbers an enum value - no effect on stored data (enums are stored by name)."""

    def __init__(self, enum: str, name: str, value: int):
        self.enum = enum
        self.name = name
        self.value = value

    def apply(self, desc):
        _find_member(_find_class(desc, self.enum), self.name)["value"] = self.value


class RunCode(Op):
    """
    A hand-written data transformation with no effect on the schema's
    shape. The data runtime it needs isn't built yet, so an older file
    whose upgrade passes through one is refused.
    """

    runtime = RUNTIME_UNSUPPORTED

    def __init__(self, name: str, description: str):
        self.name = name
        self.description = description

    def apply(self, desc):
        pass

    def runtime_entries(self):
        return [(self.runtime, "", self.name, self.description)]


class Todo(Op):
    """Left by makemigration for a change it can't decide on its own; always fails validation."""

    def __init__(self, message: str):
        self.message = message

    def apply(self, desc):
        raise MigrationError(f"unresolved TODO: {self.message}")


@dataclass
class Migration:
    from_version: str
    to_version: str
    description: str
    ops: List[Op] = field(default_factory=list)
    _path: Optional[Path] = field(default=None, repr=False)

    def label(self) -> str:
        return self._path.name if self._path else f"{self.from_version} -> {self.to_version}"


# --- Loading ---------------------------------------------------------------------

_FILE_RE = re.compile(r"^(\d{4})_[A-Za-z0-9_]+\.py$")


def load_migrations(migrations_dir: Path) -> List[Migration]:
    """Every migration in `migrations_dir`, in file-number order."""
    if not migrations_dir.is_dir():
        return []
    files = sorted(p for p in migrations_dir.iterdir() if _FILE_RE.match(p.name))
    migrations = []
    for path in files:
        spec = importlib.util.spec_from_file_location(f"migration_{path.stem}", path)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        migration = getattr(module, "migration", None)
        if not isinstance(migration, Migration):
            raise MigrationError(f"{path}: no `migration = Migration(...)`")
        migration._path = path
        migrations.append(migration)
    return migrations


# --- Checking ----------------------------------------------------------------------


def diff_summary(a: Dict[str, Any], b: Dict[str, Any], limit: int = 12) -> List[str]:
    """A short human-readable list of how descriptor `b` differs from `a`."""
    lines: List[str] = []
    a_classes = {k["name"]: k for k in _classes(a)}
    b_classes = {k["name"]: k for k in _classes(b)}
    for name in sorted(b_classes.keys() - a_classes.keys()):
        lines.append(f"+ class {name}")
    for name in sorted(a_classes.keys() - b_classes.keys()):
        lines.append(f"- class {name}")
    for name in sorted(a_classes.keys() & b_classes.keys()):
        ka, kb = a_classes[name], b_classes[name]
        ma = {m["name"]: m for m in _members(ka)} if ka["kind"] == kb["kind"] else {}
        mb = {m["name"]: m for m in _members(kb)} if ka["kind"] == kb["kind"] else {}
        if ka["kind"] != kb["kind"]:
            lines.append(f"~ class {name}: {ka['kind']} -> {kb['kind']}")
            continue
        for m in sorted(mb.keys() - ma.keys()):
            lines.append(f"+ {name}.{m}")
        for m in sorted(ma.keys() - mb.keys()):
            lines.append(f"- {name}.{m}")
        for m in sorted(ma.keys() & mb.keys()):
            if ma[m] != mb[m]:
                lines.append(f"~ {name}.{m}: {ma[m]} -> {mb[m]}")
    if len(lines) > limit:
        lines = lines[:limit] + [f"... and {len(lines) - limit} more"]
    return lines


def replay(baseline: Dict[str, Any], migrations: List[Migration]) -> List[Tuple[Migration, Dict[str, Any]]]:
    """Apply each migration's ops symbolically, returning the descriptor after each one."""
    desc = copy.deepcopy(baseline)
    results = []
    for migration in migrations:
        for index, op in enumerate(migration.ops):
            try:
                op.apply(desc)
            except MigrationError as e:
                raise MigrationError(f"{migration.label()}: op {index + 1} ({type(op).__name__}): {e}") from None
        desc["version"] = migration.to_version
        _check_references(desc, migration)
        results.append((migration, copy.deepcopy(desc)))
    return results


def _check_references(desc: Dict[str, Any], migration: Migration) -> None:
    names = {k["name"] for k in _classes(desc)}
    for klass in _classes(desc):
        if klass["kind"] == "enum":
            continue
        for f in klass["fields"]:
            if f["kind"] != "scalar" and f["type"] not in names:
                raise MigrationError(f"{migration.label()}: afterwards {klass['name']}.{f['name']} refers to missing class {f['type']}")


def check_migrations(current: Dict[str, Any], history_dir: Path, migrations_dir: Path) -> List[str]:
    """
    Every error in the migration chain: it must run from the oldest
    snapshot through every later one, each step landing exactly on that
    version's snapshot, the last on `current`.
    """
    errors: List[str] = []
    snapshots = d.read_snapshots(history_dir)
    if not snapshots:
        return errors
    try:
        migrations = load_migrations(migrations_dir)
    except MigrationError as e:
        return [str(e)]

    versions = sorted(snapshots, key=d.parse_version)
    expected_from = versions[0]
    for migration in migrations:
        if migration.from_version != expected_from:
            errors.append(f"{migration.label()}: migrates from {migration.from_version}, but the chain is at {expected_from}")
            return errors
        if migration.to_version not in snapshots:
            errors.append(f"{migration.label()}: no schema snapshot for its target version {migration.to_version}")
            return errors
        expected_from = migration.to_version
    for version in versions[1:]:
        if version not in {m.to_version for m in migrations}:
            errors.append(
                f"schema version {version} has no migration - run "
                f"`codegen --schema <schema.py> --target makemigration --name <what_changed>` and finish it"
            )
    if errors:
        return errors

    try:
        results = replay(snapshots[versions[0]]["descriptor"], migrations)
    except MigrationError as e:
        return [str(e)]
    for migration, desc in results:
        snapshot = snapshots[migration.to_version]
        if d.fingerprint(desc) != snapshot["fingerprint"]:
            diff = diff_summary(desc, snapshot["descriptor"])
            errors.append(
                f"{migration.label()} doesn't produce schema {migration.to_version}; still to account for:\n  " + "\n  ".join(diff)
            )
            return errors
    final = results[-1][1] if results else snapshots[versions[0]]["descriptor"]
    if d.fingerprint(final) != d.fingerprint(current):
        errors.append("the migration chain doesn't reach the current schema:\n  " + "\n  ".join(diff_summary(final, current)))
    return errors


def runtime_table(migrations: List[Migration]) -> List[Tuple[str, str, str, str, str, str]]:
    """(to_version, kind, class, old, new, description) for every op with a runtime effect, in chain order."""
    table = []
    for migration in migrations:
        for op in migration.ops:
            for kind, klass, old, new in op.runtime_entries():
                table.append((migration.to_version, kind, klass, old, new, migration.description))
    return table


# --- Drafting ------------------------------------------------------------------------


def _same_but_name(a: Dict[str, Any], b: Dict[str, Any]) -> bool:
    return {k: v for k, v in a.items() if k != "name"} == {k: v for k, v in b.items() if k != "name"}


def draft_ops(old: Dict[str, Any], new: Dict[str, Any], ask_rename=None) -> List[Op]:
    """
    The ops taking descriptor `old` to `new`. A removal plus an addition of
    the same shape could be a rename: `ask_rename(kind, where, old, new)`
    decides (returns True for a rename); without it a Todo is left instead.
    """
    renames: List[Op] = []
    adds: List[Op] = []
    changes: List[Op] = []
    removes: List[Op] = []
    todos: List[Op] = []

    def is_rename(kind, where, a, b):
        if ask_rename is None:
            todos.append(Todo(f"{kind} {where}: '{a}' removed and '{b}' added with the same shape - a rename, or remove + add?"))
            return None
        return ask_rename(kind, where, a, b)

    old_classes = {k["name"]: k for k in _classes(old)}
    new_classes = {k["name"]: k for k in _classes(new)}
    removed_classes = [n for n in old_classes if n not in new_classes]
    added_classes = [n for n in new_classes if n not in old_classes]
    renamed_to: Dict[str, str] = {}
    for gone in list(removed_classes):
        for came in list(added_classes):
            if _same_but_name(old_classes[gone], new_classes[came]):
                answer = is_rename("class", "", gone, came)
                if answer is None:
                    removed_classes.remove(gone)
                    added_classes.remove(came)
                elif answer:
                    renames.append(RenameClass(gone, came))
                    renamed_to[gone] = came
                    removed_classes.remove(gone)
                    added_classes.remove(came)
                break

    for name in added_classes:
        adds.append(AddClass(copy.deepcopy(new_classes[name])))
    for name in removed_classes:
        removes.append(RemoveClass(name))

    for name, new_klass in new_classes.items():
        old_name = next((o for o, n in renamed_to.items() if n == name), name)
        old_klass = old_classes.get(old_name)
        if old_klass is None or old_klass["name"] in removed_classes:
            continue
        if old_klass["kind"] != new_klass["kind"]:
            todos.append(Todo(f"class {name} changed from {old_klass['kind']} to {new_klass['kind']}"))
            continue
        enum = new_klass["kind"] == "enum"
        old_members = {m["name"]: m for m in _members(old_klass)}
        new_members = {m["name"]: m for m in _members(new_klass)}
        # A field referring to a renamed class compares against the new
        # name below - RenameClass already retargets it.
        gone_members = [m for m in old_members if m not in new_members]
        came_members = [m for m in new_members if m not in old_members]
        for gone in list(gone_members):
            for came in list(came_members):
                a = dict(old_members[gone])
                if a.get("type") in renamed_to:
                    a["type"] = renamed_to[a["type"]]
                if _same_but_name(a, new_members[came]):
                    answer = is_rename("enum value" if enum else "field", name, gone, came)
                    if answer is None:
                        gone_members.remove(gone)
                        came_members.remove(came)
                    elif answer:
                        renames.append(RenameEnumValue(name, gone, came) if enum else RenameField(name, gone, came))
                        gone_members.remove(gone)
                        came_members.remove(came)
                    break
        for came in came_members:
            member = new_members[came]
            adds.append(AddEnumValue(name, came, member["value"]) if enum else AddField(name, copy.deepcopy(member)))
        for gone in gone_members:
            removes.append(RemoveEnumValue(name, gone) if enum else RemoveField(name, gone))
        for member_name in old_members.keys() & new_members.keys():
            a = dict(old_members[member_name])
            if a.get("type") in renamed_to:
                a["type"] = renamed_to[a["type"]]
            b = new_members[member_name]
            if a != b:
                changes.append(AlterEnumValue(name, member_name, b["value"]) if enum else AlterField(name, copy.deepcopy(b)))

    return todos + renames + adds + changes + removes


def render_migration(migration_from: str, migration_to: str, description: str, ops: List[Op]) -> str:
    lines = [
        f'"""Migration {migration_from} -> {migration_to}: {description}"""',
        "",
        "from codegen.migration import *",
        "",
        "migration = Migration(",
        f"    from_version={migration_from!r},",
        f"    to_version={migration_to!r},",
        f"    description={description!r},",
        "    ops=[",
    ]
    for op in ops:
        rendered = op.render().replace("\n", "\n        ")
        lines.append(f"        {rendered},")
    lines += ["    ],", ")", ""]
    return "\n".join(lines)


def next_migration_path(migrations_dir: Path, name: str) -> Path:
    slug = re.sub(r"[^A-Za-z0-9_]+", "_", name).strip("_").lower() or "migration"
    numbers = [int(_FILE_RE.match(p.name).group(1)) for p in migrations_dir.iterdir() if _FILE_RE.match(p.name)] if migrations_dir.is_dir() else []
    return migrations_dir / f"{(max(numbers) + 1) if numbers else 1:04d}_{slug}.py"
