"""
Schema migrations (plans/NATIVE_FILE_FORMAT_RESEARCH.md §4).

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
import types
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
                    for typed in _typed(f):
                        if typed.get("type") == self.old:
                            typed["type"] = self.new

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
    # An extension's migration: its name, and the core schema version it was
    # written against (filled in by makemigration), which places it in the
    # merged core-and-extension plan.
    extension: Optional[str] = None
    depends_on_core: Optional[str] = None
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
        # From source every time, never __pycache__ (a quick edit keeping the
        # file's size would load stale bytecode).
        module = types.ModuleType(f"migration_{path.stem}")
        module.__file__ = str(path)
        exec(compile(path.read_text(), str(path), "exec"), module.__dict__)
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


def replay(
    baseline: Dict[str, Any], migrations: List[Migration], external: frozenset = frozenset(), before_op=None
) -> List[Tuple[Migration, Dict[str, Any]]]:
    """
    Apply each migration's ops symbolically, returning the descriptor after
    each one. `external` names classes outside the descriptor that fields
    may refer to (core classes, for an extension's chain); `before_op(desc,
    op)` may raise MigrationError to refuse an op.
    """
    desc = copy.deepcopy(baseline)
    results = []
    for migration in migrations:
        for index, op in enumerate(migration.ops):
            try:
                if before_op is not None:
                    before_op(desc, op)
                op.apply(desc)
            except MigrationError as e:
                raise MigrationError(f"{migration.label()}: op {index + 1} ({type(op).__name__}): {e}") from None
        desc["version"] = migration.to_version
        _check_references(desc, migration, external)
        results.append((migration, copy.deepcopy(desc)))
    return results


def _typed(field: Dict[str, Any]) -> List[Dict[str, Any]]:
    """The dicts in `field` that name a class in "type": an owner field's
    options (it stores one of several parents), else the field itself."""
    return field.get("options", []) if field["kind"] == "owner" else [field]


def _check_references(desc: Dict[str, Any], migration: Migration, external: frozenset = frozenset()) -> None:
    names = {k["name"] for k in _classes(desc)} | external
    for klass in _classes(desc):
        if klass["kind"] == "enum":
            continue
        for f in klass["fields"]:
            if f["kind"] == "scalar":
                continue
            for typed in _typed(f):
                if typed["type"] not in names:
                    raise MigrationError(f"{migration.label()}: afterwards {klass['name']}.{f['name']} refers to missing class {typed['type']}")


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


# --- Extension chains ---------------------------------------------------------------
#
# An extension's chain is checked like core's, against its own snapshots and
# descriptor, plus three rules (plans/NATIVE_FILE_FORMAT_RESEARCH.md §4.8):
# - its ops may only change its own classes;
# - each migration records the core version it was written against, never
#   decreasing along the chain and never newer than the current core;
# - a core migration that renamed a class the extension refers to has
#   already carried the extension's data along, so the extension's
#   descriptor is compared after following those renames - schema_ext.py
#   is updated, but no extension migration is needed.


def core_renames_since(core_migrations: List[Migration], since: Optional[str]) -> Dict[str, str]:
    """old class name -> current name, for every core RenameClass in a migration past `since`."""
    renames: Dict[str, str] = {}
    for migration in core_migrations:
        if since is not None and d.parse_version(migration.to_version) <= d.parse_version(since):
            continue
        for op in migration.ops:
            if isinstance(op, RenameClass):
                for old, new in list(renames.items()):
                    if new == op.old:
                        renames[old] = op.new
                renames.setdefault(op.old, op.new)
    return renames


def retarget(desc: Dict[str, Any], renames: Dict[str, str]) -> Dict[str, Any]:
    """`desc` with every reference to a renamed class following the rename."""
    if not renames:
        return desc
    desc = copy.deepcopy(desc)
    for klass in _classes(desc):
        if klass["kind"] != "enum":
            for f in klass["fields"]:
                for typed in _typed(f):
                    if typed.get("type") in renames:
                        typed["type"] = renames[typed["type"]]
    return desc


def _op_classes(op: Op) -> Tuple[List[str], List[str]]:
    """(existing classes the op changes, classes it creates)."""
    if isinstance(op, AddClass):
        return [], [op.klass["name"]]
    if isinstance(op, RenameClass):
        return [op.old], [op.new]
    if isinstance(op, RemoveClass):
        return [op.name], []
    if hasattr(op, "enum"):
        return [op.enum], []
    if hasattr(op, "klass") and isinstance(op.klass, str):
        return [op.klass], []
    return [], []


@dataclass
class ExtensionChainContext:
    """What checking an extension's chain needs to know about core."""

    core_version: str
    core_migrations: List[Migration]
    core_class_names: frozenset  # every class core has, or had in any snapshot


def check_extension_migrations(
    current: Dict[str, Any], history_dir: Path, migrations_dir: Path, extension: str, prefix: str, core: ExtensionChainContext
) -> List[str]:
    """Every error in an extension's migration chain (see the section comment)."""
    snapshots = d.read_snapshots(history_dir)
    if not snapshots:
        return []
    try:
        migrations = load_migrations(migrations_dir)
    except MigrationError as e:
        return [str(e)]

    errors: List[str] = []
    versions = sorted(snapshots, key=d.parse_version)
    expected_from = versions[0]
    previous_core = None
    for migration in migrations:
        where = migration.label()
        if migration.extension != extension:
            errors.append(f"{where}: an extension migration must say extension={extension!r}")
        if migration.depends_on_core is None:
            errors.append(f"{where}: missing depends_on_core (the core schema version it was written against)")
        else:
            dep = d.parse_version(migration.depends_on_core)
            if previous_core is not None and dep < d.parse_version(previous_core):
                errors.append(f"{where}: depends_on_core {migration.depends_on_core} is older than the previous migration's {previous_core}")
            if dep > d.parse_version(core.core_version):
                errors.append(f"{where}: depends_on_core {migration.depends_on_core} is newer than this core schema ({core.core_version})")
            previous_core = migration.depends_on_core
        if migration.from_version != expected_from:
            errors.append(f"{where}: migrates from {migration.from_version}, but the chain is at {expected_from}")
            return errors
        if migration.to_version not in snapshots:
            errors.append(f"{where}: no schema snapshot for its target version {migration.to_version}")
            return errors
        expected_from = migration.to_version
    for version in versions[1:]:
        if version not in {m.to_version for m in migrations}:
            errors.append(
                f"schema version {version} has no migration - run "
                f"`codegen --schema <schema.py> --extension <dir> --target makemigration --migrate-extension {extension} --name <what_changed>`"
            )
    if errors:
        return errors

    def own_classes_only(desc, op):
        existing, created = _op_classes(op)
        for name in existing:
            if not _has_class(desc, name):
                raise MigrationError(f"{name} isn't one of {extension}'s classes - an extension's migrations may only change its own classes")
        for name in created:
            if not name.startswith(prefix):
                raise MigrationError(f"{name} must be named with {extension}'s prefix {prefix}")

    try:
        results = replay(snapshots[versions[0]]["descriptor"], migrations, core.core_class_names, own_classes_only)
    except MigrationError as e:
        return [str(e)]
    for migration, desc in results:
        snapshot = snapshots[migration.to_version]
        if d.fingerprint(desc) != snapshot["fingerprint"]:
            diff = diff_summary(desc, snapshot["descriptor"])
            return [f"{migration.label()} doesn't produce schema {migration.to_version}; still to account for:\n  " + "\n  ".join(diff)]
    final = results[-1][1] if results else snapshots[versions[0]]["descriptor"]
    aligned_with = migrations[-1].depends_on_core if migrations else snapshots[versions[0]].get("core_version")
    final = retarget(final, core_renames_since(core.core_migrations, aligned_with))
    if d.fingerprint(final) != d.fingerprint(current):
        errors.append("the migration chain doesn't reach the current schema:\n  " + "\n  ".join(diff_summary(final, current)))
    return errors


def merged_plan(
    core_migrations: List[Migration], core_version: str, extension_chains: List[Tuple[str, List[Migration]]]
) -> List[Tuple[Migration, str]]:
    """
    (migration, extension) for every migration in merged-plan order: core
    migrations in order, each extension migration right after the core
    migration that reached its depends_on_core (before the next one),
    extensions tied at the same point in name order. Core migrations have an
    empty extension.
    """
    core_versions = [core_migrations[0].from_version] if core_migrations else [core_version]
    core_versions += [m.to_version for m in core_migrations]

    def position(version: Optional[str]) -> int:
        if version is None:
            return len(core_versions) - 1
        target = d.parse_version(version)
        return sum(1 for v in core_versions if d.parse_version(v) <= target) - 1

    keyed = []
    for index, migration in enumerate(core_migrations):
        keyed.append(((index + 1, 0, "", 0), migration, ""))
    for name, migrations in extension_chains:
        for index, migration in enumerate(migrations):
            keyed.append(((position(migration.depends_on_core), 1, name, index), migration, name))
    keyed.sort(key=lambda entry: entry[0])
    return [(migration, extension) for _, migration, extension in keyed]


def merged_runtime_table(
    core_migrations: List[Migration], core_version: str, extension_chains: List[Tuple[str, List[Migration]]]
) -> List[Tuple[str, str, str, str, str, str, str, str]]:
    """
    (to_version, kind, class, old, new, description, extension, depends_on_core)
    for every op with a runtime effect, in merged-plan order (merged_plan).
    Core rows have an empty extension.
    """
    table = []
    for migration, extension in merged_plan(core_migrations, core_version, extension_chains):
        for op in migration.ops:
            for kind, klass, old, new in op.runtime_entries():
                table.append((migration.to_version, kind, klass, old, new, migration.description, extension, migration.depends_on_core or ""))
    return table


def runtime_table(migrations: List[Migration]) -> List[Tuple[str, str, str, str, str, str]]:
    """(to_version, kind, class, old, new, description) for every op with a runtime effect, in chain order."""
    table = []
    for migration in migrations:
        for op in migration.ops:
            for kind, klass, old, new in op.runtime_entries():
                table.append((migration.to_version, kind, klass, old, new, migration.description))
    return table


# --- Drafting ------------------------------------------------------------------------


def _retargeted(member: Dict[str, Any], renamed_to: Dict[str, str]) -> Dict[str, Any]:
    """A copy of `member` with class names in renamed_to replaced."""
    member = copy.deepcopy(member)
    if "kind" in member:
        for typed in _typed(member):
            if typed.get("type") in renamed_to:
                typed["type"] = renamed_to[typed["type"]]
    return member


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
                a = _retargeted(old_members[gone], renamed_to)
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
            a = _retargeted(old_members[member_name], renamed_to)
            b = new_members[member_name]
            if a != b:
                changes.append(AlterEnumValue(name, member_name, b["value"]) if enum else AlterField(name, copy.deepcopy(b)))

    return todos + renames + adds + changes + removes


def render_migration(
    migration_from: str,
    migration_to: str,
    description: str,
    ops: List[Op],
    extension: Optional[str] = None,
    depends_on_core: Optional[str] = None,
) -> str:
    lines = [
        f'"""Migration {migration_from} -> {migration_to}: {description}"""',
        "",
        "from codegen.migration import *",
        "",
        "migration = Migration(",
    ]
    if extension is not None:
        lines += [f"    extension={extension!r},", f"    depends_on_core={depends_on_core!r},  # filled in by makemigration"]
    lines += [
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
