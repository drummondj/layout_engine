"""
Extension schemas, merged into the core schema at build time.

An extension with `schema = "schema_ext.py"` in its manifest's [contents]
supplies a module defining

    VERSION = "0.1.0"          # the extension's schema version
    def extend(schema): ...    # appends its Klasses to schema.classes

Its classes become ordinary Root classes. The rules:
- every class it adds is named with its manifest prefix, and no class or
  generated name collides with another class's;
- it changes no class it doesn't own (core or another extension's): no
  fields added, removed or altered. A class of its own may still have a
  parent= on a core class; that parent's child list is derived, never
  stored, so it is synthesized here (Field.synthesized_by) rather than
  written into the core class.
- a class of its own may own objects of a class with polymorphic owners
  (Shape) by declaring the child list on itself with owner=True:
  Field(name="shapes", type="Shape", is_list=True, is_child=True,
  owner=True). The other side - one more owner option on Shape, named
  after the class (hello_marker) - is synthesized; it stores only that
  option's name in Shape's owner column, so Shape's own fields don't
  change.

Extensions apply in the order given (dependency order, from CMake), so an
extension may own children of an earlier extension's classes.
"""

import copy
from dataclasses import dataclass
import types
from pathlib import Path
from typing import Dict, List, Optional

from codegen import descriptor as schema_descriptor
from codegen import extension_manifest
from codegen.schema import Field, Klass, Schema, to_snake_case


class ExtensionSchemaError(Exception):
    pass


@dataclass
class ExtensionSchema:
    name: str
    prefix: str
    package_version: str  # le_extension.toml's version
    schema_path: Path
    migrations: Optional[Path] = None  # the manifest's `migrations`, if set
    version: str = ""  # the schema's own VERSION, set by apply()

    @property
    def history_dir(self) -> Path:
        return self.schema_path.parent / "schema_history"

    @property
    def migrations_dir(self) -> Path:
        return self.migrations or self.schema_path.parent / "migrations"


def load(directory: Path) -> Optional[ExtensionSchema]:
    """The extension in `directory`, or None if its manifest declares no schema."""
    try:
        manifest = extension_manifest.load(Path(directory))
    except extension_manifest.ManifestError as e:
        raise ExtensionSchemaError(str(e)) from e
    if manifest.schema is None:
        return None
    return ExtensionSchema(manifest.name, manifest.prefix, manifest.version, manifest.schema, manifest.migrations)


def _state(klass: Klass):
    """What an extension may not change about a class it doesn't own."""
    return (klass.has_pool, klass.is_enum, [copy.copy(f) for f in klass.fields])


def apply(schema: Schema, extensions: List[ExtensionSchema], core_renames: Optional[Dict[str, str]] = None) -> List[str]:
    """
    Runs each extension's extend() on `schema` in order, marking the classes
    it adds and synthesizing their parents' child lists. Returns every rule
    broken (empty on success). The schema isn't linked yet. `core_renames`
    (old -> current class name, from core's migrations) turns a reference
    to a since-renamed core class into a precise message.
    """
    errors: List[str] = []
    for ext in extensions:
        before: Dict[str, tuple] = {k.name: _state(k) for k in schema.classes}
        try:
            # Compiled from source every time, never from __pycache__: an
            # edit within the same second that keeps the file's size (a
            # VERSION bump, say) would otherwise load the stale bytecode.
            module = types.ModuleType(f"le_extension_schema_{ext.name}")
            module.__file__ = str(ext.schema_path)
            exec(compile(ext.schema_path.read_text(), str(ext.schema_path), "exec"), module.__dict__)
        except Exception as e:  # the module's own error, whatever it is
            errors.append(f"extension {ext.name}: couldn't load {ext.schema_path}: {e}")
            continue
        version = getattr(module, "VERSION", None)
        if not isinstance(version, str):
            errors.append(f"extension {ext.name}: {ext.schema_path} must set VERSION = \"MAJOR.MINOR.PATCH\" (its schema version)")
            continue
        try:
            schema_descriptor.parse_version(version)
        except ValueError as e:
            errors.append(f"extension {ext.name}: {e}")
            continue
        ext.version = version
        if not callable(getattr(module, "extend", None)):
            errors.append(f"extension {ext.name}: {ext.schema_path} must define extend(schema)")
            continue
        module.extend(schema)
        errors += _check_and_mark(schema, ext, before, core_renames or {})
    return errors


def _check_and_mark(schema: Schema, ext: ExtensionSchema, before: Dict[str, tuple], core_renames: Dict[str, str]) -> List[str]:
    errors: List[str] = []
    where = f"extension {ext.name}"
    names = [k.name for k in schema.classes]
    for name in sorted(set(before) - set(names)):
        errors.append(f"{where} removed class {name}, which it doesn't own")
    seen = set()
    for name in names:
        if name in seen:
            errors.append(f"{where}: class {name} is defined twice")
        seen.add(name)

    added: List[Klass] = []
    for klass in schema.classes:
        state = before.get(klass.name)
        if state is None:
            if klass.extension is None and klass not in added:
                added.append(klass)
        elif _state(klass) != state:
            owner = klass.extension or "core"
            errors.append(
                f"{where} changed class {klass.name}, which belongs to {owner}: extensions may not add, change or remove "
                f"fields of classes they don't own - keep per-object data in a class of your own with parent= on {klass.name}"
            )

    taken = {to_snake_case(name): name for name in before}
    for klass in added:
        klass.extension = ext.name
        if not klass.name.startswith(ext.prefix) or klass.name == ext.prefix:
            errors.append(f"{where}: class {klass.name} must be named with the extension's prefix {ext.prefix} (e.g. {ext.prefix}{klass.name})")
        snake = to_snake_case(klass.name)
        if snake in taken and taken[snake] != klass.name:
            errors.append(f"{where}: class {klass.name}'s generated names collide with {taken[snake]}'s")
        taken[snake] = klass.name

    by_name = {k.name: k for k in schema.classes}
    for klass in added:
        for field in klass.fields:
            if field.is_child and field.owner:
                errors += _synthesize_owner_option(by_name, ext, klass, field)
                continue
            if field.type not in by_name and field.type in core_renames:
                errors.append(
                    f"{where}: a core migration renamed {field.type} to {core_renames[field.type]} - update {ext.schema_path.name}: "
                    f"{klass.name}.{field.name} (its stored data already follows the rename; no migration needed)"
                )
            if field.parent is None:
                continue
            parent = by_name.get(field.type)
            if parent is None or parent.extension == ext.name:
                continue  # unknown types are validation's to report; its own classes declare their child lists
            existing = next((f for f in parent.fields if f.name == field.parent), None)
            if existing is not None:
                errors.append(f"{where}: {klass.name}.{field.name} has parent=\"{field.parent}\", but {parent.name} already has a field {field.parent} - pick another name")
                continue
            child_list = Field(
                name=field.parent,
                description=f"{klass.name} objects (from the {ext.name} extension) owned by this {parent.name}",
                type=klass.name,
                is_list=True,
                is_child=True,
            )
            child_list.synthesized_by = ext.name
            parent.fields.append(child_list)
    return errors


def _synthesize_owner_option(by_name: Dict[str, Klass], ext: ExtensionSchema, klass: Klass, child_list: Field) -> List[str]:
    """The owner option on `child_list.type` that `child_list` (owner=True) declares from the owning side."""
    where = f"extension {ext.name}"
    target = by_name.get(child_list.type)
    if target is None:
        return []  # validation reports the unknown type
    if not target.get_owner_fields():
        return [f"{where}: {klass.name}.{child_list.name} has owner=True, but {target.name} has no owner fields to join - drop owner=True"]
    option = to_snake_case(klass.name)
    if any(f.name == option for f in target.fields):
        return [f"{where}: {klass.name}.{child_list.name} would add owner option {option} to {target.name}, which already has a field {option}"]
    owner_field = Field(
        name=option,
        description=f"The {klass.name} (from the {ext.name} extension) that owns this {target.name}",
        type=klass.name,
        parent=child_list.name,
        owner=True,
    )
    owner_field.synthesized_by = ext.name
    target.fields.append(owner_field)
    child_list.owner = False  # the list itself is an ordinary derived child list...
    child_list.declares_owner = True  # ...whose descriptor records the declaration
    return []
